#include "png_exporter.hpp"

#include <nodehammer/viewer/platform.hpp>

#include <sokol_gfx.h>
#include <sokol_glue.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <ios>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace nodehammer::viewer {

void PngExporter::request(PngExportSettings &settings, const RenderQualitySettings &live_quality,
                          bool scene_ready, std::string explicit_path, bool quit_when_done) {
    if (phase_ != Phase::Idle) {
        if (deps_.notifications != nullptr) {
            deps_.notifications->warning("A screenshot export is already in progress");
        }
        return;
    }
    if (!scene_ready) {
        if (deps_.notifications != nullptr) {
            deps_.notifications->error("Load a scene before exporting a screenshot");
        }
        return;
    }

    explicit_path_ = std::move(explicit_path);
    quit_when_done_ = quit_when_done;
    settings_ = settings;
    // Clamp the output and derive the internal (supersampled) resolution. Reduce
    // the supersample factor as needed so the internal target fits the backend's
    // max texture size — keeping the downscale an exact integer ratio.
    uint32_t out_w = std::clamp<uint32_t>(settings_.out_width, 16u, 16384u);
    uint32_t out_h = std::clamp<uint32_t>(settings_.out_height, 16u, 16384u);
    uint32_t ss =
        std::clamp<uint32_t>(settings_.supersample, 1u, PngExportSettings::kMaxSupersample);
    const uint32_t max_tex = static_cast<uint32_t>(sg_query_limits().max_image_size_2d);
    if (max_tex > 0) {
        out_w = std::min(out_w, max_tex);
        out_h = std::min(out_h, max_tex);
        while (ss > 1 && (out_w * ss > max_tex || out_h * ss > max_tex)) {
            --ss;
        }
    }
    settings_.out_width = out_w;
    settings_.out_height = out_h;
    settings_.supersample = ss;
    internal_w_ = out_w * ss;
    internal_h_ = out_h * ss;
    // Reflect the applied output size back to the UI-owned settings.
    settings = settings_;

    // Maxed quality snapshot for the export frames. Start from the live settings
    // to keep the user's "look" (tonemap curve, exposure, contrast, saturation,
    // sun) and force every cost/quality lever to its best.
    quality_ = live_quality;
    quality_.dynamic_render_scale = false;
    quality_.render_scale = 1.0f;
    quality_.render_scale_memory_budget_mb = 0.0f; // no memory cap for a one-shot export
    quality_.cap_fps = false;
    quality_.pause_when_static = false;
    quality_.enable_fxaa = true;
    quality_.fxaa_quality = FxaaQualityPreset::Ultra;
    quality_.enable_ao = true;
    quality_.ao_quality = AoQualityPreset::Ultra;
    quality_.ao_resolution_scale = 1.0f;
    quality_.enable_ao_denoise = true;
    quality_.enable_tonemap = true;
    quality_.debug_view = DebugView::Off;
    if (deps_.hdr_supported && deps_.hdr_supported()) {
        quality_.enable_hdr = true;
    }

    converge_count_ = 0;
    readback_started_ = false;
    readback_.reset();
    filename_ = deps_.make_filename ? deps_.make_filename() : std::string{};
    phase_ = Phase::Rendering;
    progress_ = deps_.notifications != nullptr
                    ? deps_.notifications->startProgress("Rendering screenshot...")
                    : ui::Notifications::ProgressHandle{0};
    std::println("viewer: PNG export started ({}×{} ×{} SSAA → {}×{})", out_w, out_h, ss, out_w,
                 out_h);
}

void PngExporter::preRender() {
    render_active_ = false;
    capture_ = false;
    if (phase_ != Phase::Rendering) {
        return;
    }
    // Allocate the composite output target at the export resolution, using the
    // swapchain's color/depth formats so the composite pipeline (which bakes the
    // swapchain formats) validates against it. Created here, before render()
    // begins any pass.
    sg_environment env = sglue_environment();
    sg_pixel_format color_fmt = env.defaults.color_format;
    if (color_fmt == SG_PIXELFORMAT_NONE) {
        color_fmt = SG_PIXELFORMAT_RGBA8;
    }
    sg_pixel_format depth_fmt = env.defaults.depth_format;
    if (depth_fmt == SG_PIXELFORMAT_NONE) {
        depth_fmt = SG_PIXELFORMAT_DEPTH;
    }
    if (!out_rt_.matches(internal_w_, internal_h_, color_fmt, depth_fmt)) {
        out_rt_.create(internal_w_, internal_h_, color_fmt, depth_fmt, /*for_readback=*/true);
    }
    render_active_ = true;
    // Render a handful of export-res frames so the GTAO temporal denoise (and the
    // frame-late AO history the PBR path samples) converge before we capture.
    constexpr int kConvergeFrames = 8;
    capture_ = (converge_count_ >= kConvergeFrames);
}

void PngExporter::postRender() {
    switch (phase_) {
    case Phase::Idle:
        return;
    case Phase::Rendering:
        if (capture_) {
            // The capture composite was issued this frame; wait for its GPU work
            // to drain before reading back. A few normal frames in between let
            // sokol's in-flight semaphore guarantee the capture frame completed.
            phase_ = Phase::WaitGpu;
            wait_count_ = 3;
        } else {
            ++converge_count_;
        }
        return;
    case Phase::WaitGpu:
        if (--wait_count_ <= 0) {
            phase_ = Phase::Readback;
            readback_started_ = false;
        }
        return;
    case Phase::Readback: {
        if (!readback_started_) {
            readback_started_ = true;
            if (!readback_.begin(out_rt_.color, internal_w_, internal_h_, out_rt_.color_format)) {
                finish(false, "Screenshot readback is not supported on this build");
                return;
            }
        }
        std::vector<std::uint8_t> pixels;
        const ReadbackStatus st = readback_.poll(pixels);
        if (st == ReadbackStatus::Pending) {
            return; // try again next frame
        }
        if (st != ReadbackStatus::Ready) {
            finish(false, "Screenshot GPU readback failed");
            return;
        }
        // SSAA resolve (box-downscale by the supersample factor) → PNG → deliver.
        const uint32_t ss = settings_.supersample;
        auto small = downscaleBoxRgba8(pixels, internal_w_, internal_h_, ss);
        const uint32_t out_w = internal_w_ / ss;
        const uint32_t out_h = internal_h_ / ss;
        auto png = encodePngRgba8(small, out_w, out_h);
        if (png.empty()) {
            finish(false, "Screenshot PNG encoding failed");
            return;
        }
        auto dest = deliver(png);
        if (!dest) {
            finish(false, "Failed to save screenshot");
            return;
        }
        const bool downloaded = platform::kIsWeb && explicit_path_.empty();
        finish(true, (downloaded ? "Downloaded " : "Saved ") + *dest);
        return;
    }
    }
}

std::optional<std::string> PngExporter::deliver(const std::vector<std::uint8_t> &png) {
    // Headless CLI path: write straight to the requested file.
    if (!explicit_path_.empty()) {
        std::ofstream out{explicit_path_, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::nullopt;
        }
        out.write(reinterpret_cast<const char *>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        if (!out) {
            return std::nullopt;
        }
        return explicit_path_;
    }
    // Interactive path: native writes to cwd, web triggers a download.
    if (!deps_.save_image) {
        return std::nullopt;
    }
    return deps_.save_image(filename_,
                            std::as_bytes(std::span<const std::uint8_t>{png.data(), png.size()}));
}

void PngExporter::finish(bool ok, const std::string &message) {
    if (progress_ != 0 && deps_.notifications != nullptr) {
        if (ok) {
            deps_.notifications->finishProgress(progress_, message);
        } else {
            deps_.notifications->cancelProgress(progress_);
        }
        progress_ = 0;
    }
    if (ok) {
        std::println("viewer: PNG export complete -- {}", message);
    } else {
        std::println(stderr, "viewer: PNG export failed -- {}", message);
        if (deps_.notifications != nullptr) {
            deps_.notifications->error(message);
        }
    }
    readback_.reset();
    render_active_ = false;
    capture_ = false;
    phase_ = Phase::Idle;
    // The export target can be large; free it until the next export.
    out_rt_.destroy();

    const bool quit_now = quit_when_done_;
    explicit_path_.clear();
    quit_when_done_ = false;
    if (quit_now && deps_.on_quit) {
        // Headless screenshot mode: shut the window down once the file is out.
        deps_.on_quit();
    }
}

void PngExporter::destroyTargets() {
    out_rt_.destroy();
    readback_.reset();
}

} // namespace nodehammer::viewer

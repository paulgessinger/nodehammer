#pragma once

#include "png_export_readback.hpp"
#include "scene_render_target.hpp"
#include "ui/notifications.hpp"

#include <nodehammer/viewer/png_export.hpp>
#include <nodehammer/viewer/render_quality.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nodehammer::viewer {

/// Owns the high-resolution PNG screenshot export — the `Idle → Rendering →
/// WaitGpu → Readback` state machine and the `export_*` member cluster pulled
/// out of `App::Impl`. The GPU seams (the `ImageReadback` backend, the pure
/// `downscaleBoxRgba8` / `encodePngRgba8` functions) are orchestrated here; the
/// App owns the notification surface, the platform save, the screenshot
/// filename, and the actual `render()` — those are injected as `Deps`.
///
/// render() drives the machine cooperatively: each frame the App calls
/// `preRender()` (allocate the export target + raise the render_active/capture
/// flags render() consults), then its normal `render()` (which renders at the
/// export resolution and composites the converged frame into `outTarget()`),
/// then `postRender()` (advance the machine; on the capture frame it waits for
/// GPU drain, reads back, downscales, encodes, and delivers).
class PngExporter {
  public:
    enum class Phase {
        Idle,
        Rendering, // rendering export-res frames so the AO temporal denoise converges
        WaitGpu,   // capture composite issued; waiting for its GPU work to finish
        Readback   // reading back, then downscale + encode + deliver
    };

    /// App-owned effects the exporter can't reach itself.
    struct Deps {
        ui::Notifications *notifications{nullptr};
        std::function<std::string()> make_filename; // makeScreenshotFilename
        std::function<bool()> hdr_supported;        // hdrSupported
        // Interactive delivery (native: write to cwd; web: browser download).
        // Only called when no explicit path was given.
        std::function<std::optional<std::string>(const std::string &filename,
                                                 std::span<const std::byte>)>
            save_image;
        std::function<void()> on_quit; // sapp_quit
    };
    void configure(Deps deps) { deps_ = std::move(deps); }

    /// Snapshot the live quality + settings and begin an export. `settings` is
    /// clamped in place (so the UI reflects the applied output size + reduced
    /// supersample). `scene_ready` gates the request (an uploaded scene is
    /// required). A non-empty `explicit_path` writes the PNG straight to that
    /// file (headless `--screenshot`); otherwise it goes through
    /// `deps.save_image`. `quit_when_done` quits the app once the export
    /// resolves (headless mode).
    void request(PngExportSettings &settings, const RenderQualitySettings &live_quality,
                 bool scene_ready, std::string explicit_path = {}, bool quit_when_done = false);

    /// Before render(): allocate the export target and set the flags render()
    /// reads. No-op unless a Rendering export is in flight.
    void preRender();
    /// After render(): advance the state machine; on the Readback phase, resolve
    /// SSAA → PNG → deliver.
    void postRender();

    [[nodiscard]] bool active() const { return phase_ != Phase::Idle; }
    [[nodiscard]] Phase phase() const { return phase_; }

    // Flags + resources render() consults while an export capture is in flight.
    [[nodiscard]] bool renderActive() const { return render_active_; }
    [[nodiscard]] bool capture() const { return capture_; }
    [[nodiscard]] const RenderQualitySettings &quality() const { return quality_; }
    [[nodiscard]] std::uint32_t internalWidth() const { return internal_w_; }
    [[nodiscard]] std::uint32_t internalHeight() const { return internal_h_; }
    [[nodiscard]] SceneRenderTarget &outTarget() { return out_rt_; }

    /// Free GPU resources (onCleanup).
    void destroyTargets();

  private:
    std::optional<std::string> deliver(const std::vector<std::uint8_t> &png);
    void finish(bool ok, const std::string &message);

    Deps deps_;
    Phase phase_{Phase::Idle};
    PngExportSettings settings_;    // clamped snapshot for the in-flight export
    RenderQualitySettings quality_; // maxed snapshot used while exporting
    SceneRenderTarget out_rt_;      // composite output (LDR, swapchain format)
    ImageReadback readback_;
    std::uint32_t internal_w_{0};
    std::uint32_t internal_h_{0};
    int converge_count_{0};
    int wait_count_{0};
    bool render_active_{false}; // render() targets the export resolution this frame
    bool capture_{false};       // render() also composites into out_rt_ this frame
    bool readback_started_{false};
    ui::Notifications::ProgressHandle progress_{0};
    std::string filename_;
    std::string explicit_path_;
    bool quit_when_done_{false};
};

} // namespace nodehammer::viewer

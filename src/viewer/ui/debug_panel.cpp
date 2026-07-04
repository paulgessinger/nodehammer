#include "debug_panel.hpp"

#include "../ibl.hpp"
#include "icon_font.hpp"
#include "notifications.hpp"
#include "perf_history.hpp"

#include <nodehammer/viewer/platform.hpp>

#include <imgui.h>
#include <implot.h>
#include <sokol_gfx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nodehammer::viewer::ui {
namespace {

// Series colors shared between the numeric readouts and the plot lines so the
// two read as one unit.
constexpr ImVec4 kFrameColor{0.95f, 0.77f, 0.06f, 1.f};   // amber
constexpr ImVec4 kCpuColor{0.16f, 0.63f, 0.93f, 1.f};     // blue  — CPU submit total
constexpr ImVec4 kEncodeColor{0.30f, 0.80f, 0.85f, 1.f};  // cyan  — CPU encode (offscreen)
constexpr ImVec4 kPresentColor{0.93f, 0.32f, 0.34f, 1.f}; // red   — present / GPU backpressure
constexpr ImVec4 kWaitColor{0.85f, 0.40f, 0.90f, 1.f};    // magenta — GPU wait (in encode)
constexpr ImVec4 kSceneColor{0.36f, 0.86f, 0.30f, 1.f};   // green
constexpr ImVec4 kGpuColor{0.98f, 0.55f, 0.15f, 1.f};     // orange — real GPU time (timestamps)
constexpr ImVec4 kFpsColor{0.85f, 0.85f, 0.85f, 1.f};     // light grey

const char *backendName() {
    switch (sg_query_backend()) {
    case SG_BACKEND_GLCORE:
        return "GL";
    case SG_BACKEND_GLES3:
        return "GLES3 / WebGL2";
    case SG_BACKEND_D3D11:
        return "D3D11";
    case SG_BACKEND_METAL_IOS:
        return "Metal (iOS)";
    case SG_BACKEND_METAL_MACOS:
        return "Metal (macOS)";
    case SG_BACKEND_METAL_SIMULATOR:
        return "Metal (sim)";
    case SG_BACKEND_WGPU:
        return "WebGPU";
    case SG_BACKEND_VULKAN:
        return "Vulkan";
    case SG_BACKEND_DUMMY:
        return "dummy";
    }
    return "?";
}

// A label + right-aligned, fixed-width value on one line. The value column is
// pre-formatted into a padded field by the caller so the digits don't shift the
// layout as the number changes (the default ImGui font is monospaced).
void readoutRow(const ImVec4 &color, const char *label, const char *value) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(150.f);
    ImGui::TextUnformatted(value);
}

void plotSeries(const char *label, const ScrollingBuffer &buf, const ImVec4 &color) {
    if (buf.data.empty()) {
        return;
    }
    ImPlotSpec spec;
    spec.LineColor = color;
    spec.LineWeight = 1.5f;
    // The ring buffer stores interleaved (x, y) ImVec2s; Offset tells ImPlot
    // where the oldest sample is so the wrapped line stays contiguous in time.
    spec.Offset = buf.offset;
    spec.Stride = sizeof(ImVec2);
    ImPlot::PlotLine(label, &buf.data[0].x, &buf.data[0].y, buf.data.size(), spec);
}

// Largest sample value at or after x_min (i.e. within the visible window).
float windowMax(const ScrollingBuffer &buf, float x_min) {
    float m = 0.f;
    for (const ImVec2 &p : buf.data) {
        if (p.x >= x_min) {
            m = std::max(m, p.y);
        }
    }
    return m;
}

// Mean of the samples at or after x_min. Used to steady the numeric readouts so
// the digits average over a short window instead of flickering every frame.
float windowMean(const ScrollingBuffer &buf, float x_min) {
    double sum = 0.0;
    int n = 0;
    for (const ImVec2 &p : buf.data) {
        if (p.x >= x_min) {
            sum += static_cast<double>(p.y);
            ++n;
        }
    }
    return n > 0 ? static_cast<float>(sum / n) : 0.f;
}

// Round up to a "nice" number (1, 1.5, 2, 3, 4, 5, 6, 8, 10 × 10^n) so axis
// bounds — and therefore the tick labels — land on readable values.
float niceCeil(float v) {
    if (v <= 0.f) {
        return 1.f;
    }
    const float base = std::pow(10.f, std::floor(std::log10(v)));
    const float f = v / base; // in [1, 10)
    for (const float step : {1.f, 1.5f, 2.f, 3.f, 4.f, 5.f, 6.f, 8.f}) {
        if (f <= step + 1e-4f) {
            return step * base;
        }
    }
    return 10.f * base;
}

// Per-plot smoothed peak. Persisted across frames so the y-axis ratchets up
// instantly on a spike but eases back down, instead of rescaling every frame.
struct AxisTracker {
    float smoothed = 0.f;
};

// Stable upper bound for a zero-anchored y-axis: fast attack on new peaks, slow
// exponential release toward the current window max, then a 10% headroom and a
// nice-number round-up. `min_top` guards the degenerate near-zero case.
float stableTop(AxisTracker &tr, float window_max, float min_top, float dt) {
    if (window_max > tr.smoothed) {
        tr.smoothed = window_max;
    } else {
        constexpr float kReleaseTau = 2.f; // seconds to relax toward lower peaks
        const float a = 1.f - std::exp(-std::clamp(dt, 0.f, 0.1f) / kReleaseTau);
        tr.smoothed += (window_max - tr.smoothed) * a;
    }
    return niceCeil(std::max(tr.smoothed, min_top) * 1.1f);
}

void renderPerfSection(const ViewerUiContext &ctx) {
    ImGui::SeparatorText("Performance");

    // Numeric readouts are averaged over a short trailing window so the digits
    // stay readable instead of flickering every frame. Without history (rare),
    // fall back to the instantaneous values.
    const PerfHistory *hist = ctx.perf_history;
    constexpr float kReadoutAvgSecs = 0.5f;
    const float ravg_min = hist != nullptr ? hist->t - kReadoutAvgSecs : 0.f;
    const double v_fps = hist ? windowMean(hist->fps, ravg_min) : static_cast<double>(ctx.fps);
    const double v_frame = hist ? windowMean(hist->frame_ms, ravg_min) : ctx.frame_interval_ms;
    const double v_encode = hist ? windowMean(hist->encode_ms, ravg_min) : ctx.encode_ms;
    const double v_wait = hist ? windowMean(hist->gpu_wait_ms, ravg_min) : ctx.gpu_wait_ms;
    const double v_present = hist ? windowMean(hist->present_ms, ravg_min) : ctx.present_ms;
    const double v_scene = hist ? windowMean(hist->scene_submit_ms, ravg_min) : ctx.scene_submit_ms;
    const double v_cpu = v_encode + v_present; // total submit = encode + present

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%7.1f", v_fps);
    readoutRow(kFpsColor, "FPS", buf);
    std::snprintf(buf, sizeof(buf), "%7.2f ms", v_frame);
    readoutRow(kFrameColor, "Frame", buf);
    std::snprintf(buf, sizeof(buf), "%7.2f ms", v_cpu);
    readoutRow(kCpuColor, "CPU submit", buf);
    std::snprintf(buf, sizeof(buf), "%7.2f ms", v_encode);
    readoutRow(kEncodeColor, "  encode", buf);
    // GPU wait is the in-flight-frames semaphore block, counted inside encode.
    // A large value here means GPU-bound; encode minus gpu wait is real CPU work.
    std::snprintf(buf, sizeof(buf), "%7.2f ms", v_wait);
    readoutRow(kWaitColor, "    gpu wait", buf);
    std::snprintf(buf, sizeof(buf), "%7.2f ms", v_present);
    readoutRow(kPresentColor, "  present", buf);
    std::snprintf(buf, sizeof(buf), "%7.2f ms", v_scene);
    readoutRow(kSceneColor, "Scene submit", buf);

    // Real GPU time from timestamp queries (D3D11 only today). This is the cost
    // the CPU submit rows above can't see: on D3D11 the frame stalls inside
    // sokol_app's Present(), which runs after sg_commit() where every timer here
    // has already stopped. A GPU total far above CPU submit ⇒ GPU-bound.
    const GpuPassTimings *gpu = ctx.gpu_pass_times;
    if (gpu != nullptr && gpu->valid) {
        ImGui::Spacing();
        std::snprintf(buf, sizeof(buf), "%7.2f ms", gpu->total_ms);
        readoutRow(kGpuColor, "GPU total", buf);
        for (int i = 0; i < gpu->count; ++i) {
            char label[32];
            std::snprintf(label, sizeof(label), "  %s", gpu->segments[i].label);
            std::snprintf(buf, sizeof(buf), "%7.2f ms", gpu->segments[i].ms);
            readoutRow(kGpuColor, label, buf);
        }
    }

    // Per-frame sokol draw-submission counters. The encode cost is dominated by
    // these calls, so they attribute where the CPU submit time goes.
    const RenderCallStats &cs = ctx.call_stats;
    ImGui::Spacing();
    ImGui::TextUnformatted("Draw submission / frame");
    ImGui::Text("  draws %u   bindings %u   uniforms %u", cs.num_draw, cs.num_apply_bindings,
                cs.num_apply_uniforms);
    // Pipeline switches should be ~1-2. Approaching the draw count means the
    // loop is thrashing pipeline state (groups not sorted by cull mode) — each
    // switch is an extra sg_apply_pipeline + VS-uniform re-apply.
    const bool pipeline_thrash = cs.num_draw > 4 && cs.num_apply_pipeline * 4 >= cs.num_draw;
    ImGui::TextColored(pipeline_thrash ? kPresentColor : kSceneColor, "  pipeline switches %u",
                       cs.num_apply_pipeline);
    // Metal-only: how many vertex-buffer binds actually hit the driver vs. were
    // skipped by sokol's state cache (confirms the IBL textures aren't rebound).
    if (cs.mtl_set_vertex_buffer + cs.mtl_skip_vertex_buffer > 0) {
        ImGui::Text("  vtx-buf binds %u set / %u skipped (Metal)", cs.mtl_set_vertex_buffer,
                    cs.mtl_skip_vertex_buffer);
        ImGui::Text("  pipeline-state sets %u (Metal)", cs.mtl_set_render_pipeline_state);
    }

    if (ctx.perf_history == nullptr) {
        return;
    }
    const PerfHistory &h = *ctx.perf_history;

    // Rolling window (seconds) of history kept on screen.
    constexpr float kHistorySecs = 8.f;
    constexpr ImPlotFlags kPlotFlags = ImPlotFlags_NoInputs;
    const float x_min = h.t - kHistorySecs;
    const float dt = ImGui::GetIO().DeltaTime;

    // Zero-anchored y-axes with a stabilized upper bound (see stableTop): they
    // ratchet up on spikes and ease back down, rounded to nice tick values.
    static AxisTracker timing_axis;
    const float timing_max =
        std::max({windowMax(h.frame_ms, x_min), windowMax(h.encode_ms, x_min),
                  windowMax(h.present_ms, x_min), windowMax(h.scene_submit_ms, x_min),
                  windowMax(h.gpu_total_ms, x_min)});
    const float timing_top = stableTop(timing_axis, timing_max, 2.f, dt);

    if (ImPlot::BeginPlot("Frame timing (ms)", ImVec2(-1.f, 140.f), kPlotFlags)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, 0);
        ImPlot::SetupAxisLimits(ImAxis_X1, x_min, h.t, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, timing_top, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
        plotSeries("Frame", h.frame_ms, kFrameColor);
        plotSeries("Encode", h.encode_ms, kEncodeColor);
        plotSeries("GPU wait", h.gpu_wait_ms, kWaitColor);
        plotSeries("Present", h.present_ms, kPresentColor);
        plotSeries("Scene", h.scene_submit_ms, kSceneColor);
        plotSeries("GPU", h.gpu_total_ms, kGpuColor);
        ImPlot::EndPlot();
    }

    static AxisTracker fps_axis;
    const float fps_top = stableTop(fps_axis, windowMax(h.fps, x_min), 30.f, dt);

    if (ImPlot::BeginPlot("FPS", ImVec2(-1.f, 100.f), kPlotFlags)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, 0);
        ImPlot::SetupAxisLimits(ImAxis_X1, x_min, h.t, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, fps_top, ImPlotCond_Always);
        plotSeries("FPS", h.fps, kFpsColor);
        ImPlot::EndPlot();
    }
}

} // namespace

void renderDebugPanel(bool *open, const ViewerUiContext &ctx, const UiActions &actions) {
    if (!ImGui::Begin("Debug", open)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Backbuffer: %u x %u", ctx.fb_width, ctx.fb_height);
    // Derive the scale from the actual target size so it tracks the live
    // dynamic value, not just the static slider.
    const double render_scale =
        ctx.fb_width > 0 ? static_cast<double>(ctx.scene_width) / static_cast<double>(ctx.fb_width)
                         : 0.0;
    ImGui::Text("Render target: %u x %u (scale %.2fx)", ctx.scene_width, ctx.scene_height,
                render_scale);
    if (ctx.quality.enable_ao && ctx.ao_width > 0) {
        ImGui::Text("AO target: %u x %u (scale %.2fx)", ctx.ao_width, ctx.ao_height,
                    static_cast<double>(ctx.quality.ao_resolution_scale));
    }
    ImGui::Text("Renderer: %s", backendName());
    if (sg_query_backend() == SG_BACKEND_GLES3) {
        static constexpr const char *kWebGpuCanIUseUrl = "https://caniuse.com/webgpu";
        // Sub-pixel barycentric rounding in WebGL2/ANGLE→Metal differs from
        // Metal/WebGPU's native rasterizer, so per-pixel `v_normal_world`
        // and `v_world_pos` come out infinitesimally different on GLES3.
        // Each cubemap sample (irradiance / prefilter / reflection) lands at
        // a slightly different world direction, which integrates across the
        // image into a visible global hue cast vs WebGPU. Not patchable in
        // shader code — the rasterizer choice is below the GLSL layer.
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 60, 255));
        ImGui::TextUnformatted(ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked() && actions.open_url) {
            actions.open_url(kWebGpuCanIUseUrl);
        }
        ImGui::SetItemTooltip("GLES3 / WebGL2 has output that differs from other backends. Go to \n"
                              "WebGPU if your browser supports it for optimal rendering.\n"
                              "Click the icon to open compatibility info.");
    }
    renderPerfSection(ctx);

    if constexpr (platform::kIsWeb) {
        if (ImGui::Button("Commit settings to URL") && actions.sync_browser_url) {
            actions.sync_browser_url();
        }
    }

    ImGui::Checkbox("throttle when idle", &ctx.cfg.pause_when_unfocused);

    if (ctx.ibl_settings != nullptr) {
        ImGui::SeparatorText("IBL bake");
        IblSettings &s = *ctx.ibl_settings;
        ImGui::SliderInt("BRDF samples", &s.brdf_samples, 16, 4096);
        ImGui::SliderInt("Irradiance samples", &s.irradiance_samples, 16, 4096);
        ImGui::SliderInt("Prefilter samples", &s.prefilter_samples, 16, 2048);
        ImGui::ColorEdit3("Zenith", &s.zenith_color.x);
        ImGui::ColorEdit3("Horizon", &s.horizon_color.x);
        ImGui::ColorEdit3("Ground", &s.ground_color.x);
        ImGui::DragFloat3("Sun direction", &s.sun_dir.x, 0.01f, -1.f, 1.f);
        ImGui::ColorEdit3("Sun color", &s.sun_color.x,
                          ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Sun intensity", &s.sun_intensity, 0.f, 50.f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Scalar multiplier baked into the irradiance + prefilter cubemaps. "
                              "With HDR on, values >> 1 push the disc into the tonemap shoulder.");
        ImGui::SliderFloat("Sun sharpness", &s.sun_sharpness, 1.f, 1024.f, "%.1f",
                           ImGuiSliderFlags_Logarithmic);
        if (ImGui::Button("Rebake IBL") && actions.rebake_ibl) {
            actions.rebake_ibl();
        }
    }

    if (ctx.notifications != nullptr) {
        ImGui::SeparatorText("Notifications");
        if (ImGui::Button("Info")) {
            ctx.notifications->info("Info: a short status message.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Success")) {
            ctx.notifications->success("Success: the operation completed.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Warning")) {
            ctx.notifications->warning("Warning: something looks off but we kept going.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Error")) {
            ctx.notifications->error(
                "Error: a longer message to show wrapping at one third of the viewport width, "
                "and to give you something to click on so the auto-dismiss timer pauses.");
        }
    }

    ImGui::End();
}

} // namespace nodehammer::viewer::ui

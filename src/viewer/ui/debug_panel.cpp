#include "debug_panel.hpp"

#include "../ibl.hpp"
#include "icon_font.hpp"
#include "notifications.hpp"

#include <nodehammer/viewer/platform.hpp>

#include <imgui.h>
#include <sokol_gfx.h>

namespace nodehammer::viewer::ui {
namespace {

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

} // namespace

void renderDebugPanel(bool *open, const ViewerUiContext &ctx, const UiActions &actions) {
    if (!ImGui::Begin("Debug", open)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Backbuffer: %u x %u", ctx.fb_width, ctx.fb_height);
    ImGui::Text("Renderer: %s", backendName());
    if (sg_query_backend() == SG_BACKEND_GLES3) {
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
        ImGui::SetItemTooltip("GLES3 / WebGL2 has output that differs from other backends. Go to "
                              "WebGPU if your browser supports it for optimal rendering.");
    }
    ImGui::Text("FPS: %.1f", ctx.fps);
    ImGui::Text("Frame: %.2f ms  CPU submit: %.2f ms  Scene submit: %.2f ms", ctx.frame_interval_ms,
                ctx.render_submit_ms, ctx.scene_submit_ms);

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

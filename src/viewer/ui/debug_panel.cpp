#include "debug_panel.hpp"

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
    ImGui::Text("FPS: %.1f", ctx.fps);
    ImGui::Text("Frame: %.2f ms  CPU submit: %.2f ms  Scene submit: %.2f ms", ctx.frame_interval_ms,
                ctx.render_submit_ms, ctx.scene_submit_ms);

    if constexpr (platform::kIsWeb) {
        if (ImGui::Button("Commit settings to URL") && actions.sync_browser_url) {
            actions.sync_browser_url();
        }
    }

    ImGui::Checkbox("throttle when unfocused", &ctx.cfg.pause_when_unfocused);

    if (ImGui::Button("Clear IBL cache") && actions.clear_ibl_cache) {
        actions.clear_ibl_cache();
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

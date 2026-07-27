// Standalone native window that just shows Dear ImGui's demo ("kitchen
// sink" of every widget). Independent of the nodehammer viewer/scene stack —
// links only sokol_app/sokol_gfx + Dear ImGui, no nodehammer_lib.

#include <imgui.h>
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>
#include <util/sokol_imgui.h>

namespace {

sg_pass_action g_pass_action{};

void init() {
    sg_desc desc{};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
    sg_setup(&desc);

    simgui_desc_t imgui_desc{};
    imgui_desc.logger.func = slog_func;
    simgui_setup(&imgui_desc);

    g_pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g_pass_action.colors[0].clear_value = {0.1f, 0.1f, 0.12f, 1.0f};
}

void frame() {
    simgui_frame_desc_t frame_desc{};
    frame_desc.width = sapp_width();
    frame_desc.height = sapp_height();
    frame_desc.delta_time = sapp_frame_duration();
    frame_desc.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&frame_desc);

    ImGui::ShowDemoWindow();

    sg_pass pass{};
    pass.action = g_pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    simgui_render();
    sg_end_pass();
    sg_commit();
}

void cleanup() {
    simgui_shutdown();
    sg_shutdown();
}

void event(const sapp_event *ev) { simgui_handle_event(ev); }

} // namespace

int main() {
    sapp_desc desc{};
    desc.init_cb = init;
    desc.frame_cb = frame;
    desc.event_cb = event;
    desc.cleanup_cb = cleanup;
    desc.width = 1280;
    desc.height = 800;
    desc.high_dpi = true;
    desc.window_title = "Dear ImGui Kitchen Sink";
    desc.logger.func = slog_func;
    sapp_run(&desc);
    return 0;
}

#include "imgui_backend.hpp"

#include <imgui.h>
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_log.h>
#include <util/sokol_imgui.h>

namespace nodehammer::viewer {

bool ImGui_ImplSokol_Init() {
    simgui_desc_t desc{};
    desc.no_default_font = false;
    desc.ini_filename = nullptr;
    desc.logger.func = slog_func;
    simgui_setup(&desc);
    return true;
}

void ImGui_ImplSokol_Shutdown() { simgui_shutdown(); }

void ImGui_ImplSokol_NewFrame(int fb_width, int fb_height, double delta_time, float dpi_scale) {
    simgui_frame_desc_t f{};
    f.width = fb_width;
    f.height = fb_height;
    f.delta_time = delta_time;
    f.dpi_scale = dpi_scale;
    simgui_new_frame(&f);
}

bool ImGui_ImplSokol_HandleEvent(const sapp_event *ev) { return simgui_handle_event(ev); }

void ImGui_ImplSokol_Render() { simgui_render(); }

} // namespace nodehammer::viewer

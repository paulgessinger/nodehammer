#pragma once

// Thin wrapper around util/sokol_imgui.h. Sokol's util header IS the backend
// — it owns the GPU pipeline, font texture upload, and draw submission. We
// just forward setup / per-frame / event / render calls so the viewer's
// `app.cpp` doesn't have to include the sokol headers directly.

struct sapp_event;

namespace nodehammer::viewer {

bool ImGui_ImplSokol_Init();
void ImGui_ImplSokol_Shutdown();
void ImGui_ImplSokol_NewFrame(int fb_width, int fb_height, double delta_time, float dpi_scale);
bool ImGui_ImplSokol_HandleEvent(const sapp_event *ev);
void ImGui_ImplSokol_Render();

} // namespace nodehammer::viewer

#pragma once

// Minimal Dear ImGui renderer backend for bgfx. Pattern derived from the
// canonical Richard Gale gist
// (https://gist.github.com/RichardGale/6e2b74bc42b3005e08397236e4be0fd0)
// — vendored in-tree because bgfx ships no upstream ImGui backend.
//
// Pair with imgui_impl_sdl3 for input. Init order: bgfx::init → ImGui::CreateContext
// → ImGui_ImplSDL3_InitForOther → ImGui_Implbgfx_Init.

#include <cstdint>

struct ImDrawData;

namespace nodehammer::viewer {

bool ImGui_Implbgfx_Init(uint8_t view_id);
void ImGui_Implbgfx_Shutdown();
void ImGui_Implbgfx_NewFrame();
void ImGui_Implbgfx_RenderDrawData(ImDrawData *draw_data);

} // namespace nodehammer::viewer

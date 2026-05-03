#include "icon_font.hpp"

#include <imgui.h>

#include <fa-solid-900.h>

namespace nodehammer::viewer::ui::icon_font {

namespace {
bool g_initialized = false;
} // namespace

void initialize() {
    IM_ASSERT(!g_initialized && "icon_font::initialize() called twice");
    g_initialized = true;

    ImGuiIO &io = ImGui::GetIO();
    constexpr float kBaseFontSize = 16.0f;
    constexpr float kIconFontSize = kBaseFontSize * 2.0f / 3.0f;
    static constexpr ImWchar kIconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.GlyphMinAdvanceX = kIconFontSize;
    io.Fonts->AddFontFromMemoryCompressedTTF(fa_solid_900_compressed_data,
                                             fa_solid_900_compressed_size, kIconFontSize,
                                             &icons_config, kIconRanges);
}

} // namespace nodehammer::viewer::ui::icon_font

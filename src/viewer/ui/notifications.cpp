#include "notifications.hpp"

#include <imgui.h>

#define NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW false
#include <IconsFontAwesome6.h>
#include <ImGuiNotify.hpp>
#include <fa-solid-900.h>

#include <string>

namespace nodehammer::viewer::ui::Notifications {
namespace {

void push(ImGuiToastType type, std::string_view message) {
    std::string copy{message};
    ImGui::InsertNotification({type, 3000, "%s", copy.c_str()});
}

} // namespace

void initializeFonts() {
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

void info(std::string_view message) { push(ImGuiToastType::Info, message); }

void success(std::string_view message) { push(ImGuiToastType::Success, message); }

void warning(std::string_view message) { push(ImGuiToastType::Warning, message); }

void error(std::string_view message) { push(ImGuiToastType::Error, message); }

void render() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 1.00f));
    ImGui::RenderNotifications();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);
}

} // namespace nodehammer::viewer::ui::Notifications

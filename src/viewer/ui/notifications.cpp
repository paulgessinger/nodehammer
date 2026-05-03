#include "notifications.hpp"

#include <imgui.h>

#define NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW false
#include <IconsFontAwesome6.h>
#include <ImGuiNotify.hpp>
#include <fa-solid-900.h>

#include <format>
#include <string>

namespace nodehammer::viewer::ui {
namespace {

void push(ImGuiToastType type, std::string_view message, std::size_t duration_ms) {
    std::string copy{message};
    ImGui::InsertNotification({type, static_cast<int>(duration_ms), "%s", copy.c_str()});
}

} // namespace

void Notifications::initializeFonts() {
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

void Notifications::info(std::string_view message, std::size_t duration_ms) {
    push(ImGuiToastType::Info, message, duration_ms);
}

void Notifications::success(std::string_view message, std::size_t duration_ms) {
    push(ImGuiToastType::Success, message, duration_ms);
}

void Notifications::warning(std::string_view message) { warning(message, kDefaultDuration); }

void Notifications::warning(std::string_view message, std::size_t duration_ms) {
    push(ImGuiToastType::Warning, message, duration_ms);
}

void Notifications::error(std::string_view message) { error(message, kDefaultDuration); }

void Notifications::error(std::string_view message, std::size_t duration_ms) {
    push(ImGuiToastType::Error, message, duration_ms);
}

void Notifications::diagnostic(const Diagnostic &d) {
    const auto msg = std::format("{}: {}", d.code, d.message);
    switch (d.severity) {
    case DiagnosticSeverity::Info:
        info(msg);
        return;
    case DiagnosticSeverity::Warning:
        warning(msg);
        return;
    case DiagnosticSeverity::Error:
    case DiagnosticSeverity::Fatal:
        error(msg);
        return;
    }
}

void Notifications::render() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 1.00f));
    ImGui::RenderNotifications();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);
}

} // namespace nodehammer::viewer::ui

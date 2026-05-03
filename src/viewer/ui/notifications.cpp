#include "notifications.hpp"

#include "icon_font.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>

namespace nodehammer::viewer::ui {
namespace {

constexpr float kPaddingX = 20.0f;
constexpr float kPaddingY = 20.0f;
constexpr float kPaddingBetween = 10.0f;
constexpr int kFadeMs = 150;
constexpr float kOpacity = 0.8f;

const char *iconFor(Notifications::Kind kind) {
    switch (kind) {
    case Notifications::Kind::Info:
        return ICON_FA_CIRCLE_INFO;
    case Notifications::Kind::Success:
        return ICON_FA_CIRCLE_CHECK;
    case Notifications::Kind::Warning:
        return ICON_FA_TRIANGLE_EXCLAMATION;
    case Notifications::Kind::Error:
        return ICON_FA_CIRCLE_EXCLAMATION;
    }
    return "";
}

ImVec4 colorFor(Notifications::Kind kind) {
    switch (kind) {
    case Notifications::Kind::Info:
        return ImVec4(0.0f, 0.616f, 1.0f, 1.0f);
    case Notifications::Kind::Success:
        return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    case Notifications::Kind::Warning:
        return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    case Notifications::Kind::Error:
        return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

void Notifications::push(Kind kind, std::string_view message, std::size_t duration_ms) {
    toasts_.push_back(Toast{
        .kind = kind,
        .message = std::string{message},
        .created = std::chrono::steady_clock::now(),
        .dismiss_ms = duration_ms,
    });
}

void Notifications::info(std::string_view message, std::size_t duration_ms) {
    push(Kind::Info, message, duration_ms);
}

void Notifications::success(std::string_view message, std::size_t duration_ms) {
    push(Kind::Success, message, duration_ms);
}

void Notifications::warning(std::string_view message) { warning(message, kDefaultDuration); }

void Notifications::warning(std::string_view message, std::size_t duration_ms) {
    push(Kind::Warning, message, duration_ms);
}

void Notifications::error(std::string_view message) { error(message, kDefaultDuration); }

void Notifications::error(std::string_view message, std::size_t duration_ms) {
    push(Kind::Error, message, duration_ms);
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
    const auto now = std::chrono::steady_clock::now();

    // Drop dismissed-or-expired toasts before rendering. Pinned toasts skip
    // the expiry check — they hold until the user clicks X.
    toasts_.erase(
        std::remove_if(
            toasts_.begin(), toasts_.end(),
            [&](const Toast &t) {
                if (t.dismissed) {
                    return true;
                }
                if (t.pinned) {
                    return false;
                }
                const auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - t.created).count();
                return elapsed_ms > static_cast<long long>(kFadeMs + t.dismiss_ms + kFadeMs);
            }),
        toasts_.end());

    if (toasts_.empty()) {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 wrap_size = viewport->Size;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));

    float stack_height = 0.0f;
    const float toast_width = wrap_size.x / 3.0f;
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

    for (std::size_t i = 0; i < toasts_.size(); ++i) {
        Toast &t = toasts_[i];

        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t.created).count();

        float opacity = 0.0f;
        if (t.pinned) {
            opacity = kOpacity;
        } else if (elapsed_ms < kFadeMs) {
            opacity = (static_cast<float>(elapsed_ms) / static_cast<float>(kFadeMs)) * kOpacity;
        } else if (elapsed_ms < static_cast<long long>(kFadeMs + t.dismiss_ms)) {
            opacity = kOpacity;
        } else {
            const float fade_t =
                static_cast<float>(elapsed_ms - kFadeMs - static_cast<long long>(t.dismiss_ms)) /
                static_cast<float>(kFadeMs);
            opacity = (1.0f - fade_t) * kOpacity;
        }
        opacity = std::clamp(opacity, 0.0f, kOpacity);

        char window_name[32];
        std::snprintf(window_name, sizeof(window_name), "##nh_toast_%zu", i);

        ImGui::SetNextWindowBgAlpha(opacity);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + wrap_size.x - kPaddingX,
                                       viewport->Pos.y + wrap_size.y - kPaddingY - stack_height),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        // Fixed width, auto-fit height. AlwaysAutoResize alone latches to the
        // widest content seen because the dismiss-button placement reads
        // last-frame's content width and grows it again. Pinning the width
        // here is the canonical fix.
        ImGui::SetNextWindowSizeConstraints(ImVec2(toast_width, 0.0f),
                                            ImVec2(toast_width, FLT_MAX));

        ImGui::Begin(window_name, nullptr, kFlags);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

        const ImGuiStyle &style = ImGui::GetStyle();
        const float dismiss_w = ImGui::CalcTextSize(ICON_FA_XMARK).x + style.FramePadding.x * 2.0f;
        const float text_right =
            toast_width - style.WindowPadding.x - dismiss_w - style.ItemSpacing.x;

        ImVec4 icon_color = colorFor(t.kind);
        icon_color.w = opacity;
        ImGui::TextColored(icon_color, "%s", iconFor(t.kind));
        ImGui::SameLine();

        ImGui::PushTextWrapPos(text_right);
        ImGui::TextUnformatted(t.message.data(), t.message.data() + t.message.size());
        ImGui::PopTextWrapPos();

        // Right-aligned dismiss button on the first line.
        ImGui::SameLine();
        ImGui::SetCursorPos(
            ImVec2(toast_width - style.WindowPadding.x - dismiss_w, style.WindowPadding.y));
        ImGui::PushID(static_cast<int>(i));
        const bool dismiss_clicked = ImGui::SmallButton(ICON_FA_XMARK);
        ImGui::PopID();
        if (dismiss_clicked) {
            t.dismissed = true;
        }

        // Pin-on-click: anywhere in the toast window that isn't the dismiss button.
        if (!dismiss_clicked && !t.pinned && ImGui::IsWindowHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            t.pinned = true;
        }

        stack_height += ImGui::GetWindowHeight() + kPaddingBetween;
        ImGui::End();
    }

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);
}

} // namespace nodehammer::viewer::ui

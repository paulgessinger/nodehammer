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
    case Notifications::Kind::Progress:
        return ICON_FA_SPINNER;
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
    case Notifications::Kind::Progress:
        return ImVec4(0.0f, 0.616f, 1.0f, 1.0f);
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

void Notifications::error(std::string_view message) { error(message, kManualDismiss); }

void Notifications::error(std::string_view message, std::size_t duration_ms) {
    push(Kind::Error, message, duration_ms);
}

Notifications::Toast *Notifications::findByHandle(ProgressHandle handle) {
    if (handle == 0) {
        return nullptr;
    }
    for (auto &t : toasts_) {
        if (t.handle == handle) {
            return &t;
        }
    }
    return nullptr;
}

Notifications::ProgressHandle Notifications::startProgress(std::string_view message) {
    const ProgressHandle handle = next_handle_++;
    toasts_.push_back(Toast{
        .kind = Kind::Progress,
        .message = std::string{message},
        .created = std::chrono::steady_clock::now(),
        .dismiss_ms = kDefaultDuration,
        .handle = handle,
    });
    return handle;
}

void Notifications::updateProgress(ProgressHandle handle, float fraction,
                                   std::string_view message) {
    Toast *t = findByHandle(handle);
    if (t == nullptr || t->kind != Kind::Progress) {
        return;
    }
    t->progress = std::clamp(fraction, 0.0f, 1.0f);
    if (!message.empty()) {
        t->message.assign(message);
    }
}

void Notifications::finishProgress(ProgressHandle handle, std::string_view final_message) {
    Toast *t = findByHandle(handle);
    if (t == nullptr || t->kind != Kind::Progress) {
        return;
    }
    t->kind = Kind::Success;
    t->progress = 1.0f;
    t->dismiss_ms = kDefaultDuration;
    if (!final_message.empty()) {
        t->message.assign(final_message);
    }
    // Reset the auto-hide anchor so the countdown starts now. Pull it back
    // by one fade-in window so the swap to the success state does not
    // flicker through opacity 0.
    t->created = std::chrono::steady_clock::now() - std::chrono::milliseconds(kFadeMs);
    t->hover_anchor.reset();
}

void Notifications::cancelProgress(ProgressHandle handle) {
    Toast *t = findByHandle(handle);
    if (t == nullptr || t->kind != Kind::Progress) {
        return;
    }
    t->dismissed = true;
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

    // Drop dismissed-or-expired toasts before rendering. Pinned toasts,
    // sticky toasts (dismiss_ms == 0, e.g. errors by default), and
    // still-running progress toasts skip the expiry check — they hold
    // until the user clicks X or until completion repoints the timer.
    toasts_.erase(
        std::remove_if(
            toasts_.begin(), toasts_.end(),
            [&](const Toast &t) {
                if (t.dismissed) {
                    return true;
                }
                if (t.pinned || t.dismiss_ms == kManualDismiss || t.kind == Kind::Progress) {
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

        const bool sticky = t.pinned || t.dismiss_ms == kManualDismiss || t.kind == Kind::Progress;

        float opacity = 0.0f;
        if (t.kind == Kind::Progress) {
            // Progress toasts snap straight to full opacity — the
            // fade-in looks like a flicker once the progress bar (which
            // is meant to be readable immediately) is showing through to
            // the scene behind it.
            opacity = kOpacity;
        } else if (elapsed_ms < kFadeMs) {
            opacity = (static_cast<float>(elapsed_ms) / static_cast<float>(kFadeMs)) * kOpacity;
        } else if (sticky || elapsed_ms < static_cast<long long>(kFadeMs + t.dismiss_ms)) {
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

        // In-flight progress bar, rendered below the message line so the
        // first row stays the icon + message + dismiss layout the rest of
        // the toasts share. NewLine drops the cursor to the next line at
        // the left edge — we just placed the dismiss button, which left
        // the cursor pinned to the top-right. The bar disappears once
        // finishProgress flips the kind to Success.
        if (t.kind == Kind::Progress) {
            ImGui::NewLine();
            ImGui::ProgressBar(t.progress, ImVec2(-FLT_MIN, 0.f));
        }

        const bool window_hovered = ImGui::IsWindowHovered();

        // Pin-on-click: anywhere in the toast window that isn't the dismiss button.
        if (!dismiss_clicked && !t.pinned && window_hovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            t.pinned = true;
        }

        // Hover-pause: while the cursor is over the toast, advance the
        // `created` anchor in lockstep with `now`, freezing elapsed_ms
        // so the auto-hide / fade-out timer does not progress. Click
        // (above) latches this into a permanent pin.
        if (window_hovered) {
            if (t.hover_anchor) {
                t.created += (now - *t.hover_anchor);
            }
            t.hover_anchor = now;
        } else {
            t.hover_anchor.reset();
        }

        stack_height += ImGui::GetWindowHeight() + kPaddingBetween;
        ImGui::End();
    }

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);
}

} // namespace nodehammer::viewer::ui

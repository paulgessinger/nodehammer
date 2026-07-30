#pragma once

#include <ir/diagnostics.hpp>
#include <viewer/log_sink.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::viewer::ui {

/// Toast notification surface. Owns its toast queue; one instance per App.
///
/// Implements `LogSink` so it can be passed to `ProjectFs::setLogSink` to
/// receive backend warnings/errors as toasts.
///
/// Font setup lives in `icon_font::initialize()` — call that once during
/// ImGui init before constructing or rendering any `Notifications`.
class Notifications : public LogSink {
  public:
    constexpr static std::size_t kDefaultDuration = 3000;
    /// Sentinel for `dismiss_ms` meaning "do not auto-hide; user must
    /// click the X button". Used by `error()` by default and by progress
    /// toasts while their work is in flight.
    constexpr static std::size_t kManualDismiss = 0;

    Notifications() = default;
    Notifications(const Notifications &) = delete;
    Notifications &operator=(const Notifications &) = delete;

    void info(std::string_view message, std::size_t duration_ms = kDefaultDuration);
    void success(std::string_view message, std::size_t duration_ms = kDefaultDuration);
    void warning(std::string_view message) override;
    void warning(std::string_view message, std::size_t duration_ms);
    /// Errors default to manual-dismiss only — they are usually actionable
    /// and should not vanish before the user has read them. Pass an explicit
    /// `duration_ms` if you want the error to auto-hide.
    void error(std::string_view message) override;
    void error(std::string_view message, std::size_t duration_ms);

    /// Surface a diagnostic as a toast, mapping its severity to the
    /// appropriate kind. The toast text is prefixed with the NH code
    /// (e.g. "NH0007: rule sets material = 'support' …"). Info/Warning/
    /// Error/Fatal map to info/warning/error toasts respectively.
    void diagnostic(const ir::Diagnostic &d);

    /// Handle returned by `startProgress` and used to route subsequent
    /// `updateProgress` / `finishProgress` calls. 0 is reserved as "no
    /// handle"; valid handles are always non-zero.
    using ProgressHandle = std::size_t;

    /// Open a new in-flight progress toast. The toast renders with a
    /// spinner icon plus a determinate progress bar; it is sticky (does
    /// not auto-hide) until `finishProgress` or `cancelProgress`.
    /// Multiple progress toasts may be live at once — they stack like any
    /// other notification.
    ProgressHandle startProgress(std::string_view message);

    /// Advance the bar. `fraction` is clamped to [0,1]. If `message` is
    /// non-empty it replaces the toast's text (useful for "(50/100
    /// nodes)" updates each frame). Calls with an unknown handle are
    /// silently ignored — convenient when wiring poll-driven producers.
    void updateProgress(ProgressHandle handle, float fraction, std::string_view message = {});

    /// Mark the work complete. The toast morphs into a success-style
    /// notification (green checkmark, optional updated message) and the
    /// auto-hide countdown starts from this moment.
    void finishProgress(ProgressHandle handle, std::string_view final_message = {});

    /// Drop an in-flight progress toast (e.g. on build failure where the
    /// failure itself is already surfaced as an error toast).
    void cancelProgress(ProgressHandle handle);

    void render();

    /// True while any toast is still live (fading in/out, counting down, or a
    /// sticky progress/error). Lets the frame loop know the overlay is still
    /// animating and shouldn't be frozen by on-demand rendering.
    [[nodiscard]] bool hasActiveToasts() const { return !toasts_.empty(); }

    enum class Kind { Info, Success, Warning, Error, Progress };

  private:
    struct Toast {
        Kind kind;
        std::string message;
        std::chrono::steady_clock::time_point created;
        std::size_t dismiss_ms;
        bool pinned = false;
        bool dismissed = false;

        // Progress state. Only meaningful while `kind == Kind::Progress`;
        // `finishProgress` rewrites `kind` to `Success` and resets
        // `created` so the standard fade/dismiss path takes over.
        ProgressHandle handle = 0;
        float progress = 0.0f;

        // Hover-pause anchor. While the cursor is over this toast,
        // `created` is shifted forward by each frame's delta so the
        // visible auto-hide timer freezes. Reset to nullopt when the
        // cursor leaves.
        std::optional<std::chrono::steady_clock::time_point> hover_anchor = std::nullopt;
    };

    void push(Kind kind, std::string_view message, std::size_t duration_ms);
    Toast *findByHandle(ProgressHandle handle);

    std::vector<Toast> toasts_;
    ProgressHandle next_handle_{1};
};

} // namespace nodehammer::viewer::ui

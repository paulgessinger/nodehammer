#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/log_sink.hpp>

#include <chrono>
#include <cstddef>
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

    Notifications() = default;
    Notifications(const Notifications &) = delete;
    Notifications &operator=(const Notifications &) = delete;

    void info(std::string_view message, std::size_t duration_ms = kDefaultDuration);
    void success(std::string_view message, std::size_t duration_ms = kDefaultDuration);
    void warning(std::string_view message) override;
    void warning(std::string_view message, std::size_t duration_ms);
    void error(std::string_view message) override;
    void error(std::string_view message, std::size_t duration_ms);

    /// Surface a diagnostic as a toast, mapping its severity to the
    /// appropriate kind. The toast text is prefixed with the NH code
    /// (e.g. "NH0007: rule sets material = 'support' …"). Info/Warning/
    /// Error/Fatal map to info/warning/error toasts respectively.
    void diagnostic(const Diagnostic &d);

    void render();

    enum class Kind { Info, Success, Warning, Error };

  private:
    struct Toast {
        Kind kind;
        std::string message;
        std::chrono::steady_clock::time_point created;
        std::size_t dismiss_ms;
        bool pinned = false;
        bool dismissed = false;
    };

    void push(Kind kind, std::string_view message, std::size_t duration_ms);

    std::vector<Toast> toasts_;
};

} // namespace nodehammer::viewer::ui

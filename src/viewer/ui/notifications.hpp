#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/log_sink.hpp>

#include <string_view>

namespace nodehammer::viewer::ui {

/// Toast notification surface. Wraps the process-global ImGuiNotify
/// backend; the instance shape exists so subsystems can be wired
/// explicitly (App owns one) and so future per-instance state
/// (recent-error buffer, dedupe, test capture) has a home.
///
/// Implements `LogSink` so it can be passed to `ProjectFs::setLogSink`
/// to receive backend warnings/errors as toasts.
class Notifications : public LogSink {
  public:
    constexpr static std::size_t kDefaultDuration = 3000;

    /// Installs the icon font into the global ImGui font atlas. Must run
    /// once during ImGui initialisation, before any frame.
    static void initializeFonts();

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
};

} // namespace nodehammer::viewer::ui

#pragma once

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
    /// Installs the icon font into the global ImGui font atlas. Must run
    /// once during ImGui initialisation, before any frame.
    static void initializeFonts();

    Notifications() = default;
    Notifications(const Notifications &) = delete;
    Notifications &operator=(const Notifications &) = delete;

    void info(std::string_view message);
    void success(std::string_view message);
    void warning(std::string_view message) override;
    void error(std::string_view message) override;

    void render();
};

} // namespace nodehammer::viewer::ui

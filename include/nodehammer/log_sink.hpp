#pragma once

#include <string_view>

namespace nodehammer {

/// Push-mode error/warning channel. Subsystems that produce diagnostics
/// (project filesystems today; build pipeline and importers in the future)
/// take an optional `LogSink *` and emit at the moment a problem occurs,
/// instead of stashing a string for the consumer to poll.
///
/// The default no-op bodies let subsystems be wired without a sink and stay
/// silent. Sink lifetime must outlive the subsystem holding the pointer.
class LogSink {
  public:
    virtual ~LogSink() = default;

    virtual void warning(std::string_view) {}
    virtual void error(std::string_view) {}
};

} // namespace nodehammer

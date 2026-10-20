#pragma once

#include <viewer/viewer_messages.hpp>

#include <mutex>
#include <vector>

namespace nodehammer::viewer {

/// Typed mailbox between the viewer thread and an external controller.
/// Producers may submit commands and consume events from other threads. The App
/// executes commands on its own thread after releasing the mailbox lock. Keep this shared object
/// alive rather than accessing App from server threads.
class ViewerConnection {
  public:
    virtual ~ViewerConnection() = default;

    void submit(commands::Command command);
    [[nodiscard]] std::vector<commands::Command> takeCommands();
    void publish(events::Event event);
    [[nodiscard]] std::vector<events::Event> takeEvents();

    /// Nonblocking transport housekeeping on the viewer thread, including while
    /// rendering is idle. A server using its own thread needs no override.
    virtual void poll() {}

  private:
    std::mutex mutex_;
    std::vector<commands::Command> commands_;
    std::vector<events::Event> events_;
};

} // namespace nodehammer::viewer

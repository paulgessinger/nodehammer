#include <viewer/viewer_connection.hpp>

#include <utility>

namespace nodehammer::viewer {

void ViewerConnection::submit(commands::Command command) {
    const std::lock_guard lock{mutex_};
    commands_.push_back(std::move(command));
}

std::vector<commands::Command> ViewerConnection::takeCommands() {
    const std::lock_guard lock{mutex_};
    return std::exchange(commands_, {});
}

void ViewerConnection::publish(events::Event event) {
    const std::lock_guard lock{mutex_};
    // A slow consumer needs the latest pose, not every intermediate animation
    // frame. Never coalesce across a response or a scene lifecycle event.
    if (std::holds_alternative<events::CameraChanged>(event) && !events_.empty() &&
        std::holds_alternative<events::CameraChanged>(events_.back())) {
        events_.back() = std::move(event);
    } else {
        events_.push_back(std::move(event));
    }
}

std::vector<events::Event> ViewerConnection::takeEvents() {
    const std::lock_guard lock{mutex_};
    // Move the queued batch out and leave an empty queue, under the same lock.
    return std::exchange(events_, {});
}

} // namespace nodehammer::viewer

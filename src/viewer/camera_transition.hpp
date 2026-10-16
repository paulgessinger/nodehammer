#pragma once

#include <viewer/camera.hpp>

#include <chrono>

namespace nodehammer::viewer {

/// Platform-independent orbit-pose transition, with no UI or messaging dependencies.
/// Time is measured from start(), so waking an idle renderer does not skip the ease.
class CameraTransition {
  public:
    using Clock = std::chrono::steady_clock;

    /// Start or replace a 0.4-second transition. Only pose fields are interpolated.
    void start(const Camera &from, const Camera &to, Clock::time_point now);
    void cancel() { active_ = false; }
    [[nodiscard]] bool active() const { return active_; }

    /// Apply the pose at `now`, retaining FOV and deriving clip planes from the scene.
    /// Does nothing after cancellation or completion.
    void update(Camera &camera, float scene_radius, Clock::time_point now);

  private:
    bool active_{false};
    Clock::time_point started_{};
    Camera from_;
    Camera to_;
};

} // namespace nodehammer::viewer

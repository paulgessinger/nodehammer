#include <viewer/camera_transition.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nodehammer::viewer {

void CameraTransition::start(const Camera &from, const Camera &to, Clock::time_point now) {
    from_ = from;
    to_ = to;
    from_.sanitize();
    to_.sanitize();
    started_ = now;
    active_ = true;
}

void CameraTransition::update(Camera &camera, float scene_radius, Clock::time_point now) {
    if (!active_) {
        return;
    }
    constexpr float kDurationSeconds = 0.4f;
    const float elapsed = std::chrono::duration<float>(now - started_).count();
    const float t = std::clamp(elapsed / kDurationSeconds, 0.f, 1.f);
    const float s = t * t * (3.f - 2.f * t);
    camera.target = glm::mix(from_.target, to_.target, s);
    camera.distance = std::exp(std::lerp(std::log(from_.distance), std::log(to_.distance), s));
    const float dyaw = std::remainder(to_.yaw - from_.yaw, 2.f * std::numbers::pi_v<float>);
    camera.yaw = from_.yaw + dyaw * s;
    camera.pitch = std::lerp(from_.pitch, to_.pitch, s);
    camera.projection = to_.projection;
    camera.sanitize();
    camera.dolly(1.f, scene_radius);
    if (t >= 1.f) {
        active_ = false;
    }
}

} // namespace nodehammer::viewer

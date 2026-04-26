#include <nodehammer/viewer/camera.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace nodehammer::viewer {

namespace {
constexpr float k_pitch_limit = 1.553343f; // ~89° — keep away from gimbal lock at the poles
}

glm::vec3 Camera::eye() const {
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const glm::vec3 dir{cp * sy, sp, cp * cy};
    return target + dir * distance;
}

glm::mat4 Camera::view() const { return glm::lookAt(eye(), target, glm::vec3{0.f, 1.f, 0.f}); }

glm::mat4 Camera::proj(float aspect, bool homogeneous_depth, bool reversed_z) const {
    const float f = glm::radians(fov_deg);
    glm::mat4 p = glm::perspective(f, aspect, near_plane, far_plane);
    if (!homogeneous_depth) {
        // Convert from [-1, 1] to [0, 1] depth: z' = (z + 1) / 2.
        const glm::mat4 remap = {
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
            {0.f, 0.f, 0.5f, 0.f},
            {0.f, 0.f, 0.5f, 1.f},
        };
        p = remap * p;
    }
    if (reversed_z) {
        // [0,1] backends:  z' = 1 - z   → near→1, far→0
        // [-1,1] GL:       z' = -z      → near→1, far→-1
        // glm matrices are column-major: [col][row].
        glm::mat4 flip(1.f);
        flip[2][2] = -1.f;
        if (!homogeneous_depth) {
            flip[3][2] = 1.f;
        }
        p = flip * p;
    }
    return p;
}

void Camera::orbit(float dx_radians, float dy_radians) {
    yaw -= dx_radians;
    pitch = std::clamp(pitch + dy_radians, -k_pitch_limit, k_pitch_limit);
}

void Camera::dolly(float factor) {
    // factor = 1 means no change; >1 zooms out, <1 zooms in.
    distance *= factor;
    if (scene_radius > 0.f) {
        // Don't let the user dolly straight through the scene — clamp distance
        // to a tiny fraction of the bounding-sphere radius. (We can't use
        // `near_plane * k` here the way the old path did: with the new sizing
        // `near_plane ≈ distance - scene_radius`, so that clamp would snap
        // distance back up whenever the camera sat well outside the scene.)
        distance = std::max(distance, scene_radius * 1e-3f);
        // Hug the geometry: near just in front of the closest point, far just
        // past the furthest. With reversed-Z this gives ~6 orders of magnitude
        // of usable depth precision instead of the ~3 you get from a wide
        // distance-relative pair.
        const float pad = scene_radius * 1.1f;
        near_plane = std::max(distance - pad, distance * 1e-4f);
        near_plane = std::max(near_plane, 1e-3f);
        far_plane = distance + pad;
    } else {
        near_plane = std::max(distance * 1e-3f, 1e-4f);
        far_plane = std::max(far_plane, distance * 100.f);
        distance = std::max(distance, near_plane * 8.f);
    }
}

void Camera::pan(float dx_world, float dy_world) {
    const glm::vec3 fwd = glm::normalize(target - eye());
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3{0.f, 1.f, 0.f}));
    const glm::vec3 up = glm::cross(right, fwd);
    target += right * dx_world + up * dy_world;
}

void Camera::frame_bounds(const glm::vec3 &min, const glm::vec3 &max, float margin) {
    const glm::vec3 centre = 0.5f * (min + max);
    const glm::vec3 extent = 0.5f * (max - min);
    const float radius = std::max({extent.x, extent.y, extent.z, 1.f});
    const float vfov = glm::radians(fov_deg);
    const float fit = radius / std::tan(0.5f * vfov);
    target = centre;
    distance = fit * margin;
    // Bounding-sphere radius (use the longest diagonal half-extent so the
    // near/far envelope still encloses corners when the camera orbits).
    scene_radius = glm::length(extent);
    const float pad = scene_radius * 1.1f;
    near_plane = std::max(distance - pad, distance * 1e-4f);
    near_plane = std::max(near_plane, 1e-3f);
    far_plane = distance + pad;
}

} // namespace nodehammer::viewer

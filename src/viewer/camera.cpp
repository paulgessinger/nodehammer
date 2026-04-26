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

glm::mat4 Camera::proj(float aspect, bool homogeneous_depth) const {
    const float f = glm::radians(fov_deg);
    glm::mat4 p = glm::perspective(f, aspect, near_plane, far_plane);
    if (homogeneous_depth) {
        // glm::perspective produces [0, 1] depth by default (with
        // GLM_FORCE_DEPTH_ZERO_TO_ONE) or [-1, 1] otherwise. Since we don't
        // set GLM_FORCE_*, glm gives [-1, 1] — which is what GL/GLES expect.
        // For non-homogeneous backends (Metal/D3D/Vulkan), remap z.
    } else {
        // Convert from [-1, 1] to [0, 1] depth: z' = (z + 1) / 2.
        // This matrix is composed onto the projection.
        const glm::mat4 remap = {
            {1.f, 0.f, 0.f, 0.f},
            {0.f, 1.f, 0.f, 0.f},
            {0.f, 0.f, 0.5f, 0.f},
            {0.f, 0.f, 0.5f, 1.f},
        };
        p = remap * p;
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
    // Keep near/far roughly proportional to the camera distance so depth
    // precision stays usable across the full zoom range. Without this, near
    // gets pinned to the value chosen at frame_bounds() and once you dolly in
    // a lot the near plane is far inside the geometry, which causes z-fighting
    // on the central pieces.
    near_plane = std::max(distance * 1e-3f, 1e-4f);
    far_plane = std::max(far_plane, distance * 100.f);
    // Floor the minimum distance at a bit more than near so the camera can't
    // be moved past the front of its own clip volume.
    distance = std::max(distance, near_plane * 8.f);
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
    // Scale the near/far planes with the scene size so depth precision stays
    // usable across both tiny synthetic scenes and ODD (where geometry is
    // expressed in millimetres and the bbox is ~10⁴ units across).
    near_plane = std::max(distance * 1e-3f, 1e-3f);
    far_plane = distance * 100.f;
}

} // namespace nodehammer::viewer

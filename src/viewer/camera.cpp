#include <nodehammer/viewer/camera.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nodehammer::viewer {

namespace {
constexpr float k_pitch_limit = 1.553343f; // ~89° — keep away from gimbal lock at the poles
constexpr float k_fov_min_deg = 1.f;
constexpr float k_fov_max_deg = 179.f;
constexpr float k_min_near = 1e-3f;
constexpr float k_min_distance = 1e-4f;

// Wrap into (-π, π].
float wrapPi(float a) {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float two_pi = 2.f * pi;
    if (!std::isfinite(a)) {
        return 0.f;
    }
    a = std::fmod(a + pi, two_pi);
    if (a <= 0.f) {
        a += two_pi;
    }
    return a - pi;
}

// Replace non-finite with a fallback; otherwise return v.
float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
} // namespace

bool Camera::sanitize() {
    const Camera before = *this;

    target.x = finiteOr(target.x, 0.f);
    target.y = finiteOr(target.y, 0.f);
    target.z = finiteOr(target.z, 0.f);

    distance = finiteOr(distance, 10.f);
    distance = std::max(distance, k_min_distance);

    yaw = wrapPi(yaw);
    pitch = std::clamp(finiteOr(pitch, 0.f), -k_pitch_limit, k_pitch_limit);

    if (projection != ProjectionMode::Perspective && projection != ProjectionMode::Orthographic) {
        projection = ProjectionMode::Perspective;
    }

    fov_deg = std::clamp(finiteOr(fov_deg, 55.f), k_fov_min_deg, k_fov_max_deg);

    near_plane = finiteOr(near_plane, 0.05f);
    if (projection == ProjectionMode::Perspective) {
        // Perspective requires near > 0 (the projection diverges at the eye).
        near_plane = std::max(near_plane, k_min_near);
    }
    // Orthographic accepts a negative near (the clip plane sits behind the
    // eye), which is required when the user dollies in past the scene
    // centre — geometry on the camera side of the target would otherwise
    // be clipped even though it's visually in-frame.
    far_plane = finiteOr(far_plane, 1000.f);
    if (far_plane <= near_plane) {
        far_plane = near_plane + std::max(std::abs(near_plane) * 1000.f, 1.f);
    }

    return target != before.target || distance != before.distance || yaw != before.yaw ||
           pitch != before.pitch || projection != before.projection || fov_deg != before.fov_deg ||
           near_plane != before.near_plane || far_plane != before.far_plane;
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
    glm::mat4 p(1.f);
    if (projection == ProjectionMode::Orthographic) {
        const float half_height = std::max(distance * std::tan(0.5f * f), k_min_distance);
        const float half_width = half_height * aspect;
        p = glm::ortho(-half_width, half_width, -half_height, half_height, near_plane, far_plane);
    } else {
        p = glm::perspective(f, aspect, near_plane, far_plane);
    }
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
    yaw = wrapPi(yaw - dx_radians);
    pitch = std::clamp(pitch + dy_radians, -k_pitch_limit, k_pitch_limit);
}

void Camera::dolly(float factor, float scene_radius) {
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
        // distance-relative pair. The 1.5× multiplier gives ~50% scene-radius
        // clearance in front of the closest point — close enough to the
        // camera that scene-relative widgets and overlays don't pop, while
        // still well above the precision floor.
        const float pad = scene_radius * 1.5f;
        if (projection == ProjectionMode::Orthographic) {
            // Orthographic dolly shrinks `distance` to zoom in, which moves
            // the eye toward the target. Without a negative near, geometry
            // on the camera-side of the target gets clipped the moment
            // distance < scene_radius. glm::ortho accepts a negative near
            // (the clip plane sits behind the eye), so just envelope the
            // bounding sphere symmetrically around the target.
            near_plane = distance - pad;
            far_plane = distance + pad;
        } else {
            near_plane = std::max(distance - pad, distance * 1e-4f);
            near_plane = std::max(near_plane, k_min_near);
            far_plane = distance + pad;
        }
    } else {
        near_plane = std::max(distance * 1e-3f, k_min_distance);
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

float Camera::frameBounds(const glm::vec3 &min, const glm::vec3 &max, float margin) {
    const glm::vec3 centre = 0.5f * (min + max);
    const glm::vec3 extent = 0.5f * (max - min);
    const float radius = std::max({extent.x, extent.y, extent.z, 1.f});
    const float vfov = glm::radians(fov_deg);
    const float fit = radius / std::tan(0.5f * vfov);
    target = centre;
    distance = fit * margin;
    // Bounding-sphere radius (use the longest diagonal half-extent so the
    // near/far envelope still encloses corners when the camera orbits).
    const float scene_radius = glm::length(extent);
    const float pad = scene_radius * 1.5f;
    if (projection == ProjectionMode::Orthographic) {
        // See the matching branch in dolly() — ortho needs a (possibly
        // negative) near so the camera-side half of the scene isn't clipped.
        near_plane = distance - pad;
    } else {
        near_plane = std::max(distance - pad, distance * 1e-4f);
        near_plane = std::max(near_plane, k_min_near);
    }
    far_plane = distance + pad;
    sanitize();
    return scene_radius;
}

} // namespace nodehammer::viewer

#pragma once

#include <glm/glm.hpp>

namespace nodehammer::viewer {

/// Simple orbit / arc-ball camera. State is the spherical coordinate around a
/// target point; view/proj matrices are derived on demand. Designed for
/// inspection of a static scene rather than first-person navigation.
struct Camera {
    glm::vec3 target{0.f, 0.f, 0.f};
    float distance{10.f};
    float yaw{0.f};   // radians; rotation around world Y
    float pitch{0.f}; // radians; rotation around camera-right axis
    float fov_deg{55.f};
    float near_plane{0.05f};
    float far_plane{1000.f};
    /// Bounding-sphere radius of the framed scene. Set by frame_bounds() and
    /// used by dolly() to size near/far so they hug the visible geometry.
    /// Zero means "unknown" — we fall back to the old distance-relative sizing.
    float scene_radius{0.f};

    [[nodiscard]] glm::vec3 eye() const;
    [[nodiscard]] glm::mat4 view() const;
    /// Builds a projection matrix; `homogeneous_depth` should match the
    /// backend's clip-space convention (OpenGL/GLES use [-1, 1], everything
    /// else uses [0, 1]). When `reversed_z` is true, near maps to the FAR
    /// depth value and far maps to the NEAR — this dramatically improves
    /// depth precision when paired with `GREATER_EQUAL` compare and a depth
    /// clear of 0.0.
    [[nodiscard]] glm::mat4 proj(float aspect, bool homogeneous_depth,
                                 bool reversed_z = false) const;

    /// Mouse-driven orbit. Inputs are screen-space deltas in pixels; the
    /// caller's sensitivity multiplier is baked in.
    void orbit(float dx_radians, float dy_radians);
    /// Mouse-wheel zoom; positive `factor` zooms in, negative zooms out.
    void dolly(float factor);
    /// Pan the target in the camera's right/up plane.
    void pan(float dx_world, float dy_world);

    /// Frame an axis-aligned bounding box: target = centre, distance fits
    /// the bbox in view at the current FOV with a small margin.
    void frame_bounds(const glm::vec3 &min, const glm::vec3 &max, float margin = 1.2f);
};

} // namespace nodehammer::viewer

#pragma once

#include <viewer/camera.hpp>

#include <nlohmann/json_fwd.hpp>

#include <optional>

namespace nodehammer::viewer {

/// JSON vocabulary for a camera pose, shared by every web-facing surface that
/// describes the orbit camera: the `camera` object of the web shell's
/// `nh_viewer_start` options (`src/web/viewer_main.cpp`) and the embed-mode
/// postMessage protocol (`docs/viewer-embed.md`). Keys mirror the
/// `?cameraTargetX=…&cameraYaw=…` query parameters minus the `camera` prefix:
///
///   {"targetX", "targetY", "targetZ", "distance", "yawDeg", "pitchDeg",
///    "projection": "perspective" | "orthographic"}
///
/// Angles are degrees on the wire and radians in `Camera`. `fov_deg`, the clip
/// planes and every other derived field stay out of the exchange — they are
/// re-derived against the loaded scene (see `Camera::dolly`).
///
/// `cameraFromJson` requires all six numeric fields (a partial pose is
/// ambiguous, matching the CLI's `--camera-*` mutual-requirement check) and a
/// positive `distance`; it returns nullopt otherwise. `projection` is optional
/// and defaults to perspective; `"ortho"` is accepted as a synonym.
[[nodiscard]] std::optional<Camera> cameraFromJson(const nlohmann::json &j);
[[nodiscard]] nlohmann::json cameraToJson(const Camera &camera);

/// The pose fields `cameraToJson` emits, compared exactly. Used to decide
/// whether a camera change is worth broadcasting to an embedding host.
[[nodiscard]] bool sameCameraPose(const Camera &a, const Camera &b);

} // namespace nodehammer::viewer

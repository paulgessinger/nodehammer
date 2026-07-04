#pragma once

#include <nodehammer/viewer/camera.hpp>         // ProjectionMode
#include <nodehammer/viewer/render_quality.hpp> // RenderQualitySettings

#include <glm/vec3.hpp>

#include <cstdint>

namespace nodehammer::viewer {

/// GPU limits the render-scale caps need, passed in as plain data so the
/// controller carries no sokol dependency.
struct SgLimits {
    std::uint32_t max_image_size_2d{0};
};

/// A snapshot of the camera fields the motion detector compares. Kept in the
/// same types as `Camera` so the equality check is bit-exact (the original code
/// compared `camera.yaw != dyn_prev_yaw`, etc.).
struct CameraSnapshot {
    glm::vec3 target{0.f, 0.f, 0.f};
    float yaw{0.f};
    float pitch{0.f};
    float distance{0.f};
    float fov{0.f};
    ProjectionMode proj{ProjectionMode::Perspective};

    bool operator==(const CameraSnapshot &) const = default;
};

/// Owns the viewer's adaptive render-scale state and logic — the `dyn_*`
/// cluster and the memory-budget clamp that used to live inline in
/// `App::Impl::render()`. Pulled out as a headless, unit-testable controller:
/// it takes the live quality settings, a camera snapshot, and per-frame timing
/// as plain data, and returns the render scale to use this frame.
///
/// The controller holds the highest scale in `[render_scale_min,
/// render_scale_max]` that meets the target frame time while the camera moves
/// (asymmetric drop-fast / climb-slow EMA with hold/climb locks), and jumps to
/// `render_scale_max` once the view settles. The result is then capped against
/// the backend's max texture size and the offscreen-target memory budget.
class DynamicRenderScale {
  public:
    /// Everything `update()` needs beyond the quality settings + camera, all as
    /// plain data (no sokol / ImGui / clock calls) so the controller is
    /// deterministic and testable.
    struct Inputs {
        /// Last frame's wall time in ms (the adaptive EMA's input).
        double frame_ms{0.0};
        /// A monotonic timestamp in seconds, used only for elapsed-time
        /// comparisons (hold / climb-lock / settle delay). The App passes the
        /// sokol-time clock; tests pass a synthetic one.
        double now_seconds{0.0};
        /// Whether any ImGui widget is active this frame (a held slider counts
        /// as interaction, exactly like a camera move).
        bool ui_active{false};
        std::uint32_t fb_w{0};
        std::uint32_t fb_h{0};
        SgLimits limits{};
        /// Per-pixel byte cost of the resolution-scaling offscreen targets,
        /// computed by the caller from the live backend pixel formats + AO
        /// settings. Folded into the memory-budget clamp here; pass 0 to skip
        /// the clamp.
        float bytes_per_scene_px{0.f};
    };

    /// Compute the render scale for this frame and advance the internal
    /// EMA/lock/settle state. Also refreshes the camera snapshot every call —
    /// even when dynamic scaling is off — so toggling it back on doesn't see a
    /// false change.
    float update(const RenderQualitySettings &q, const CameraSnapshot &cam, const Inputs &in);

    /// The scale returned by the most recent `update()`.
    [[nodiscard]] float current() const { return scale_; }

    /// Whether the most recent `update()` saw interaction this frame (a camera
    /// change or an active ImGui widget). Computed unconditionally — valid even
    /// when dynamic scaling is off — so callers can gate the on-demand render /
    /// idle logic on it (the former render()-local `interacting`).
    [[nodiscard]] bool interacting() const { return interacting_; }

  private:
    // Formerly the App::Impl `dyn_*` members (defaults preserved verbatim).
    float scale_{1.0f};
    float motion_scale_{0.5f};
    double frame_ms_ema_{0.0};
    bool was_moving_{false};
    double settle_anchor_sec_{0.0};
    double climb_lock_sec_{0.0};
    double last_scale_change_sec_{0.0};
    bool interacting_{false};
    CameraSnapshot prev_{};
};

} // namespace nodehammer::viewer

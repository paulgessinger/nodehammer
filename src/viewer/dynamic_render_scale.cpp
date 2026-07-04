#include <nodehammer/viewer/dynamic_render_scale.hpp>

#include <algorithm>
#include <cmath>

namespace nodehammer::viewer {

float DynamicRenderScale::update(const RenderQualitySettings &q, const CameraSnapshot &cam,
                                 const Inputs &in) {
    // Detect interaction this frame. Two sources, both of which re-render the
    // scene and should drop resolution: a camera change (orbit/pan/zoom/fov/
    // projection) and any active ImGui widget. The camera snapshot is refreshed
    // every frame — even when dynamic scaling is off — so toggling it back on
    // doesn't see a false change.
    const bool cam_changed = !(cam == prev_);
    prev_ = cam;
    const bool interacting = cam_changed || in.ui_active;
    interacting_ = interacting;

    const auto clamp_scale = [](float s) { return s < 0.25f ? 0.25f : (s > 4.0f ? 4.0f : s); };
    float render_scale;
    if (q.dynamic_render_scale) {
        float lo = clamp_scale(q.render_scale_min);
        float hi = clamp_scale(q.render_scale_max);
        if (lo > hi) {
            const float t = lo;
            lo = hi;
            hi = t;
        }
        // While the camera moves the scale stays low; once it settles it jumps
        // straight to `hi`. The settle transition is a single step (not a ramp
        // through intermediate resolutions) because each step reallocates the
        // offscreen targets *and* resets the AO temporal history — a staircase
        // of AO re-converges on a still image reads as flicker.
        constexpr double kSettleDelaySeconds = 0.2;
        if (interacting) {
            if (q.adaptive_render_scale) {
                // Closed-loop: hold the highest scale in [lo, hi] that meets the
                // target frame time. frame_ms is last frame's wall time; smooth
                // it so a single hitch doesn't yank the scale. React
                // asymmetrically — drop fast when over budget, probe up slowly —
                // and rate-limit changes so we don't reallocate every frame near
                // the budget boundary.
                const double target_ms = 1000.0 / std::max(15.0f, q.render_scale_target_fps);
                if (!was_moving_) {
                    // Interaction just started: resume from the last sustainable
                    // in-motion scale rather than dropping to the floor and
                    // re-climbing the staircase. Re-seed the average at target and
                    // clear the climb/hold locks so the controller can react right
                    // away, and so the slow full-res settled frame we just showed
                    // doesn't drag the controller down.
                    frame_ms_ema_ = target_ms;
                    climb_lock_sec_ = 0.0;
                    last_scale_change_sec_ = 0.0;
                } else {
                    constexpr double kAlpha = 0.25;           // EMA smoothing
                    constexpr double kClimbLockSeconds = 1.5; // pause up-probing after overshoot
                    // Minimum wall time between applied scale changes, in either
                    // direction. The controller still samples every frame, but it
                    // only reallocates the offscreen targets this often — so a
                    // steady orbit holds a resolution instead of churning through
                    // fine steps. A severe overshoot bypasses this to recover from
                    // a real hitch immediately.
                    constexpr double kScaleHoldSeconds = 0.4;
                    frame_ms_ema_ = frame_ms_ema_ * (1.0 - kAlpha) + in.frame_ms * kAlpha;
                    const bool hold_elapsed =
                        (in.now_seconds - last_scale_change_sec_) > kScaleHoldSeconds;
                    if (frame_ms_ema_ > target_ms * 1.6) {
                        // Severely over budget — drop hard immediately, bypassing
                        // the hold so a real hitch recovers at once, and stop
                        // probing upward: we just found the ceiling.
                        motion_scale_ -= 0.5f;
                        climb_lock_sec_ = in.now_seconds;
                        last_scale_change_sec_ = in.now_seconds;
                    } else if (frame_ms_ema_ > target_ms * 1.15) {
                        // Mildly over budget — coarsen, but no more often than the
                        // hold so we don't reallocate every frame near the
                        // boundary. Lock upward probing regardless.
                        if (hold_elapsed) {
                            motion_scale_ -= 0.125f;
                            last_scale_change_sec_ = in.now_seconds;
                        }
                        climb_lock_sec_ = in.now_seconds;
                    } else if (hold_elapsed &&
                               (in.now_seconds - climb_lock_sec_) > kClimbLockSeconds) {
                        // Under budget, or vsync-capped at it — probe finer. If
                        // this step overshoots, the branch above pulls it back and
                        // locks probing, so it settles at the sustainable scale.
                        motion_scale_ += 0.125f;
                        last_scale_change_sec_ = in.now_seconds;
                    }
                }
                motion_scale_ = motion_scale_ < lo ? lo : (motion_scale_ > hi ? hi : motion_scale_);
                scale_ = motion_scale_;
            } else {
                // Non-adaptive: fixed floor while moving (blurry-but-smooth).
                scale_ = lo;
            }
            settle_anchor_sec_ = in.now_seconds;
        } else if ((in.now_seconds - settle_anchor_sec_) >= kSettleDelaySeconds) {
            // Settled and not interacting (auto-orbit counts as interaction, so
            // it never lands here): jump to `hi` unconditionally — the still
            // image is allowed to exceed the frame-time budget to maximize
            // fidelity, since slowness no longer costs interactivity.
            scale_ = hi;
        }
        was_moving_ = interacting;
        render_scale = scale_;
    } else {
        render_scale = clamp_scale(q.render_scale);
        scale_ = render_scale;
    }

    // Don't let a high settled scale (up to 4x) push the offscreen target past
    // the backend's max texture size on a large window. Cap uniformly so the
    // aspect ratio is preserved.
    const float max_tex = static_cast<float>(in.limits.max_image_size_2d);
    if (max_tex > 0.f && in.fb_w > 0 && in.fb_h > 0) {
        const float cap =
            std::min(max_tex / static_cast<float>(in.fb_w), max_tex / static_cast<float>(in.fb_h));
        if (render_scale > cap) {
            render_scale = cap;
        }
    }

    // Clamp the render scale to the offscreen-target memory budget. The
    // per-pixel byte cost (bytes_per_scene_px) is computed by the caller from
    // the live backend pixel formats; the budget arithmetic is pure. See the
    // note in ensureSceneTarget for which targets are counted.
    if (q.render_scale_memory_budget_mb > 0.f && in.fb_w > 0 && in.fb_h > 0 &&
        in.bytes_per_scene_px > 0.f) {
        const double budget_bytes =
            static_cast<double>(q.render_scale_memory_budget_mb) * 1024.0 * 1024.0;
        const double base_px = static_cast<double>(in.fb_w) * static_cast<double>(in.fb_h);
        // total_bytes(scale) = base_px * scale² * bytes_per_scene_px ≤ budget.
        const double max_scale_sq = budget_bytes / (base_px * in.bytes_per_scene_px);
        float budget_cap = static_cast<float>(std::sqrt(std::max(0.0, max_scale_sq)));
        if (budget_cap < 0.25f) {
            budget_cap = 0.25f; // never strangle below the hard floor, even on a tiny budget
        }
        if (render_scale > budget_cap) {
            render_scale = budget_cap;
        }
    }

    scale_ = render_scale;
    return render_scale;
}

} // namespace nodehammer::viewer

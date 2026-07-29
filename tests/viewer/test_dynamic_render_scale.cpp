// Headless unit coverage for DynamicRenderScale — the adaptive render-scale
// controller extracted from App::Impl::render(). It carries no sokol/GPU
// dependency (GPU limits + per-pixel byte cost are passed in as plain data), so
// the whole EMA/lock/settle state machine and the caps are testable here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <viewer/dynamic_render_scale.hpp>

using namespace nodehammer::viewer;
using Catch::Matchers::WithinAbs;

namespace {

RenderQualitySettings baseQuality() {
    RenderQualitySettings q;
    q.dynamic_render_scale = false;
    q.render_scale = 1.0f;
    q.render_scale_min = 0.25f;
    q.render_scale_max = 4.0f;
    q.render_scale_target_fps = 60.0f;
    q.render_scale_memory_budget_mb = 0.0f; // clamp off unless a test sets it
    return q;
}

DynamicRenderScale::Inputs makeInputs(double frame_ms, double now_seconds, bool ui_active) {
    DynamicRenderScale::Inputs in;
    in.frame_ms = frame_ms;
    in.now_seconds = now_seconds;
    in.ui_active = ui_active;
    in.fb_w = 1920;
    in.fb_h = 1080;
    in.limits = {0};             // texture cap off unless a test sets it
    in.bytes_per_scene_px = 0.f; // memory clamp off unless a test sets it
    return in;
}

// A moving frame: a camera whose yaw changes every call, so cam_changed is true.
CameraSnapshot movingCam(double now) {
    CameraSnapshot c;
    c.yaw = static_cast<float>(now) + 1.0f;
    return c;
}

} // namespace

TEST_CASE("DynamicRenderScale: static path clamps render_scale to [0.25, 4]",
          "[dynamic_render_scale]") {
    DynamicRenderScale d;
    RenderQualitySettings q = baseQuality(); // dynamic off
    CameraSnapshot cam;

    q.render_scale = 2.0f;
    REQUIRE_THAT(d.update(q, cam, makeInputs(16.0, 0.0, false)), WithinAbs(2.0f, 1e-5f));

    q.render_scale = 9.0f; // above max
    REQUIRE_THAT(d.update(q, cam, makeInputs(16.0, 0.1, false)), WithinAbs(4.0f, 1e-5f));

    q.render_scale = 0.01f; // below min
    REQUIRE_THAT(d.update(q, cam, makeInputs(16.0, 0.2, false)), WithinAbs(0.25f, 1e-5f));
}

TEST_CASE("DynamicRenderScale: memory budget caps the scale", "[dynamic_render_scale]") {
    DynamicRenderScale d;
    RenderQualitySettings q = baseQuality();
    q.render_scale = 4.0f;
    q.render_scale_memory_budget_mb = 16.0f; // budget

    CameraSnapshot cam;
    DynamicRenderScale::Inputs in = makeInputs(16.0, 0.0, false);
    in.fb_w = 1024;
    in.fb_h = 1024;
    in.bytes_per_scene_px = 16.0f;
    // cap = sqrt(budget_bytes / (base_px * bytes_per_px))
    //     = sqrt(16*1024*1024 / (1024*1024 * 16)) = sqrt(1) = 1.0
    REQUIRE_THAT(d.update(q, cam, in), WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("DynamicRenderScale: max-texture-size caps the scale", "[dynamic_render_scale]") {
    DynamicRenderScale d;
    RenderQualitySettings q = baseQuality();
    q.render_scale = 4.0f;

    CameraSnapshot cam;
    DynamicRenderScale::Inputs in = makeInputs(16.0, 0.0, false);
    in.fb_w = 4000;
    in.fb_h = 2000;
    in.limits = {4096};
    // cap = min(4096/4000, 4096/2000) = 4096/4000 = 1.024
    REQUIRE_THAT(d.update(q, cam, in), WithinAbs(1.024f, 1e-4f));
}

TEST_CASE("DynamicRenderScale: non-adaptive floors while moving, jumps to max when settled",
          "[dynamic_render_scale]") {
    DynamicRenderScale d;
    RenderQualitySettings q = baseQuality();
    q.dynamic_render_scale = true;
    q.adaptive_render_scale = false;
    q.render_scale_min = 0.5f;
    q.render_scale_max = 2.0f;

    // Moving: fixed floor (render_scale_min).
    d.update(q, movingCam(0.0), makeInputs(16.0, 0.0, false));
    REQUIRE_THAT(d.update(q, movingCam(0.1), makeInputs(16.0, 0.1, false)), WithinAbs(0.5f, 1e-5f));

    // Hold still (same camera, no UI): before the settle delay elapses the scale
    // is unchanged; after 0.2s it jumps straight to render_scale_max.
    CameraSnapshot still;
    still.yaw = 3.0f;
    d.update(q, still, makeInputs(16.0, 1.0, false)); // change from moving cam → interaction
    REQUIRE_THAT(d.update(q, still, makeInputs(16.0, 1.1, false)),
                 WithinAbs(0.5f, 1e-5f)); // 0.1s < 0.2s settle delay
    REQUIRE_THAT(d.update(q, still, makeInputs(16.0, 1.4, false)),
                 WithinAbs(2.0f, 1e-5f)); // settled → max
}

TEST_CASE("DynamicRenderScale: adaptive climbs slowly under budget, drops fast over budget",
          "[dynamic_render_scale]") {
    RenderQualitySettings q = baseQuality();
    q.dynamic_render_scale = true;
    q.adaptive_render_scale = true;
    q.render_scale_min = 0.25f;
    q.render_scale_max = 4.0f;
    q.render_scale_target_fps = 60.0f; // target_ms ≈ 16.67

    SECTION("climb is slow (≤ 0.125 per applied step) and converges toward max") {
        DynamicRenderScale d;
        // Fast frames (1 ms) spaced 2s apart so the hold (0.4s) and climb lock
        // (1.5s) always elapse → one +0.125 step per frame from the 0.5 default.
        float prev = d.update(q, movingCam(0.0), makeInputs(1.0, 0.0, false)); // seeds, was_moving
        for (int i = 1; i <= 3; ++i) {
            const double now = 2.0 * i;
            const float s = d.update(q, movingCam(now), makeInputs(1.0, now, false));
            REQUIRE(s - prev <= 0.125f + 1e-4f); // never climbs faster than one step
            REQUIRE(s >= prev);                  // monotonic up under budget
            prev = s;
        }
        // Keep driving fast frames — it converges to the max, never overshoots.
        for (int i = 4; i <= 60; ++i) {
            const double now = 2.0 * i;
            d.update(q, movingCam(now), makeInputs(1.0, now, false));
        }
        REQUIRE_THAT(d.current(), WithinAbs(4.0f, 1e-4f));
    }

    SECTION("a single severe over-budget frame drops much more than a climb step") {
        DynamicRenderScale d;
        // Climb up to a comfortable mid-scale first.
        d.update(q, movingCam(0.0), makeInputs(1.0, 0.0, false));
        float climbed = 0.5f;
        for (int i = 1; i <= 6; ++i) {
            const double now = 2.0 * i;
            climbed = d.update(q, movingCam(now), makeInputs(1.0, now, false));
        }
        REQUIRE(climbed > 1.0f); // we have headroom above the floor

        // One severely-over-budget frame (200 ms ≫ target) drops immediately,
        // bypassing the hold — a ~0.5 cut, far larger than a 0.125 climb step.
        const double now = 2.0 * 6 + 0.05;
        const float dropped = d.update(q, movingCam(now), makeInputs(200.0, now, false));
        REQUIRE(climbed - dropped >= 0.5f - 1e-4f);
    }
}

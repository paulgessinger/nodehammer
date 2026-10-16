#include <viewer/camera_transition.hpp>
#include <viewer/config.hpp>
#include <viewer/viewer_connection.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace nodehammer::viewer;
using Catch::Approx;
using namespace std::chrono_literals;

TEST_CASE("camera transition measures time from the request, not an idle frame") {
    Camera camera;
    camera.distance = 10.f;
    camera.fov_deg = 40.f;
    Camera target = camera;
    target.target.x = 20.f;
    target.distance = 40.f;
    target.fov_deg = 90.f;
    target.projection = ProjectionMode::Orthographic;
    CameraTransition transition;
    const auto requested = CameraTransition::Clock::time_point{} + 10s;
    transition.start(camera, target, requested);
    transition.update(camera, 1.f, requested);
    CHECK(camera.distance == Approx(10.f));
    CHECK(transition.active());
    transition.update(camera, 1.f, requested + 200ms);
    CHECK(camera.target.x == Approx(10.f));
    CHECK(camera.distance == Approx(20.f));
    CHECK(camera.fov_deg == Approx(40.f));
    CHECK(camera.projection == ProjectionMode::Orthographic);
    CHECK(camera.near_plane == Approx(17.f));
    CHECK(camera.far_plane == Approx(23.f));
    transition.update(camera, 1.f, requested + 400ms);
    CHECK(camera.target.x == Approx(20.f));
    CHECK(camera.distance == Approx(40.f));
    CHECK_FALSE(transition.active());
}

TEST_CASE("camera transition takes the short yaw arc and can be cancelled or replaced") {
    Camera camera;
    camera.yaw = glm::radians(170.f);
    Camera target = camera;
    target.yaw = glm::radians(-170.f);
    CameraTransition transition;
    const auto start = CameraTransition::Clock::time_point{};
    transition.start(camera, target, start);
    transition.update(camera, 1.f, start + 200ms);
    CHECK(std::abs(glm::degrees(camera.yaw)) == Approx(180.f));
    transition.cancel();
    const float cancelled_yaw = camera.yaw;
    transition.update(camera, 1.f, start + 1s);
    CHECK(camera.yaw == cancelled_yaw);
    CHECK_FALSE(transition.active());
    transition.start(camera, target, start + 1s);
    Camera replacement = camera;
    replacement.target.y = 30.f;
    transition.start(camera, replacement, start + 1100ms);
    transition.update(camera, 1.f, start + 1500ms);
    CHECK(camera.target.y == Approx(30.f));
    CHECK(camera.yaw == Approx(cancelled_yaw));
    CHECK_FALSE(transition.active());
}

TEST_CASE("surface presentation is independent of external control") {
    Config cfg;
    cfg.connection = std::make_shared<ViewerConnection>();
    CHECK_FALSE(cfg.presentation == PresentationMode::Surface);
    cfg.presentation = PresentationMode::Surface;
    CHECK(cfg.presentation == PresentationMode::Surface);
    CHECK(cfg.connection);
}

#include <nodehammer/viewer/app_state.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/constants.hpp>

using namespace nodehammer::viewer;

TEST_CASE("viewer config state round-trips through TOML") {
    Config cfg;
    cfg.cull_back = false;
    cfg.pause_when_unfocused = false;
    cfg.auto_orbit = true;
    cfg.auto_orbit_speed_deg = 22.5f;
    cfg.angle_cut = true;
    cfg.shader_angle_cut = false;
    cfg.angle_cut_start_deg = 12.0f;
    cfg.angle_cut_end_deg = 123.0f;
    cfg.enable_pbr = false;

    Camera camera;
    camera.target = {1.f, 2.f, 3.f};
    camera.distance = 42.f;
    camera.yaw = glm::half_pi<float>();
    camera.pitch = -0.25f;
    camera.fov_deg = 45.f;
    camera.near_plane = 0.1f;
    camera.far_plane = 500.f;

    auto state = viewerConfigStateFrom(cfg, camera);
    state.show_project = false;
    state.show_debug = false;

    auto parsed = viewerConfigStateFromToml(viewerConfigStateToToml(state));
    REQUIRE(parsed.has_value());

    Config restored_cfg;
    Camera restored_camera;
    applyViewerConfigState(*parsed, restored_cfg, &restored_camera);

    CHECK(parsed->show_project == false);
    CHECK(parsed->show_status == true);
    CHECK(parsed->show_view == true);
    CHECK(parsed->show_debug == false);
    CHECK(restored_cfg.cull_back == false);
    CHECK(restored_cfg.pause_when_unfocused == false);
    CHECK(restored_cfg.auto_orbit == true);
    CHECK(restored_cfg.auto_orbit_speed_deg == 22.5f);
    CHECK(restored_cfg.angle_cut == true);
    CHECK(restored_cfg.shader_angle_cut == false);
    CHECK(restored_cfg.angle_cut_start_deg == 12.0f);
    CHECK(restored_cfg.angle_cut_end_deg == 123.0f);
    CHECK(restored_cfg.enable_pbr == false);
    CHECK(restored_camera.target == camera.target);
    CHECK(restored_camera.distance == camera.distance);
    CHECK(restored_camera.yaw == Catch::Approx(camera.yaw));
    CHECK(restored_camera.pitch == Catch::Approx(camera.pitch));
    CHECK(restored_camera.fov_deg == camera.fov_deg);
    CHECK(restored_camera.near_plane == camera.near_plane);
    CHECK(restored_camera.far_plane == camera.far_plane);
}

TEST_CASE("invalid viewer config state TOML is rejected") {
    CHECK_FALSE(viewerConfigStateFromToml("[view").has_value());
}

TEST_CASE("startup overrides apply after persisted viewer config fields") {
    ViewerConfigState persisted;
    persisted.cull_back = false;
    persisted.auto_orbit = true;
    persisted.angle_cut = true;
    persisted.enable_pbr = false;

    Config cfg;
    cfg.cull_back = true;
    cfg.auto_orbit = false;
    cfg.angle_cut = false;
    cfg.enable_pbr = true;

    applyViewerConfigState(persisted, cfg, nullptr);
    ConfigStartupOverrides overrides;
    overrides.cull_back = true;
    overrides.enable_pbr = true;
    applyViewerStartupOverrides(overrides, cfg, nullptr);

    CHECK(cfg.cull_back == true);
    CHECK(cfg.auto_orbit == true);
    CHECK(cfg.angle_cut == true);
    CHECK(cfg.enable_pbr == true);
}

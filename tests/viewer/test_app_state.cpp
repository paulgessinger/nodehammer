#include <viewer/app_state.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/constants.hpp>

using namespace nodehammer::viewer;

TEST_CASE("viewer config state round-trips through TOML") {
    Config cfg;
    cfg.cull = CullOverride::ForceNoCull;
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
    camera.projection = ProjectionMode::Orthographic;
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
    CHECK(restored_cfg.cull == CullOverride::ForceNoCull);
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
    CHECK(restored_camera.projection == camera.projection);
    CHECK(restored_camera.fov_deg == camera.fov_deg);
    CHECK(restored_camera.near_plane == camera.near_plane);
    CHECK(restored_camera.far_plane == camera.far_plane);
}

TEST_CASE("invalid viewer config state TOML is rejected") {
    CHECK_FALSE(viewerConfigStateFromToml("[view").has_value());
}

TEST_CASE("active build-root keys round-trip through TOML") {
    ViewerConfigState state;
    state.root_config_key = "detector/full.toml";
    state.root_geometry_key = "geo/odd.nhb.zst";

    auto parsed = viewerConfigStateFromToml(viewerConfigStateToToml(state));
    REQUIRE(parsed.has_value());
    CHECK(parsed->root_config_key == "detector/full.toml");
    CHECK(parsed->root_geometry_key == "geo/odd.nhb.zst");
}

TEST_CASE("unset build-root keys stay empty (fall back to archive manifest)") {
    ViewerConfigState state;
    auto parsed = viewerConfigStateFromToml(viewerConfigStateToToml(state));
    REQUIRE(parsed.has_value());
    CHECK(parsed->root_config_key.empty());
    CHECK(parsed->root_geometry_key.empty());
}

TEST_CASE("render quality state round-trips through TOML") {
    RenderQualitySettings quality;
    quality.render_scale = 1.5f;
    quality.dynamic_render_scale = true;
    quality.adaptive_render_scale = false;
    quality.render_scale_target_fps = 72.f;
    quality.render_scale_min = 0.4f;
    quality.render_scale_max = 3.0f;
    quality.render_scale_memory_budget_mb = 768.f;
    quality.cap_fps = true;
    quality.pause_when_static = false;
    quality.enable_hdr = false;
    quality.enable_tonemap = false;
    quality.enable_material_prefilter = false;
    quality.material_prefilter_scale = 1.7f;
    quality.material_prefilter_band = 4.2f;
    quality.lod_hull_enable = true;
    quality.lod_hull_force = true;
    quality.lod_hull_screen_px = 96.f;
    quality.lod_hull_band_px = 12.f;
    quality.enable_fxaa = false;
    quality.fxaa_subpix = 0.5f;
    quality.fxaa_edge_threshold = 0.2f;
    quality.fxaa_edge_threshold_min = 0.05f;
    quality.fxaa_quality = FxaaQualityPreset::Medium;
    quality.enable_ao = false;
    quality.ao_intensity = 0.6f;
    quality.ao_radius = 0.2f;
    quality.ao_thickness = 1.6f;
    quality.ao_quality = AoQualityPreset::High;
    quality.ao_resolution_scale = 0.5f;
    quality.enable_ao_denoise = false;
    quality.debug_view = DebugView::LinearDepth;
    quality.overdraw_range = 48.f;
    quality.exposure_stops = -1.0f;
    quality.tonemap_mode = TonemapMode::Reinhard;
    quality.contrast = 1.3f;
    quality.saturation = 0.8f;
    quality.enable_background = false;

    auto parsed = renderQualityStateFromToml(renderQualityStateToToml(quality));
    REQUIRE(parsed.has_value());
    CHECK(parsed->render_scale == quality.render_scale);
    CHECK(parsed->dynamic_render_scale == quality.dynamic_render_scale);
    CHECK(parsed->adaptive_render_scale == quality.adaptive_render_scale);
    CHECK(parsed->render_scale_target_fps == quality.render_scale_target_fps);
    CHECK(parsed->render_scale_min == quality.render_scale_min);
    CHECK(parsed->render_scale_max == quality.render_scale_max);
    CHECK(parsed->render_scale_memory_budget_mb == quality.render_scale_memory_budget_mb);
    CHECK(parsed->cap_fps == quality.cap_fps);
    CHECK(parsed->pause_when_static == quality.pause_when_static);
    CHECK(parsed->enable_hdr == quality.enable_hdr);
    CHECK(parsed->enable_tonemap == quality.enable_tonemap);
    CHECK(parsed->enable_material_prefilter == quality.enable_material_prefilter);
    CHECK(parsed->material_prefilter_scale == quality.material_prefilter_scale);
    CHECK(parsed->material_prefilter_band == quality.material_prefilter_band);
    CHECK(parsed->lod_hull_enable == quality.lod_hull_enable);
    CHECK(parsed->lod_hull_force == quality.lod_hull_force);
    CHECK(parsed->lod_hull_screen_px == quality.lod_hull_screen_px);
    CHECK(parsed->lod_hull_band_px == quality.lod_hull_band_px);
    CHECK(parsed->enable_fxaa == quality.enable_fxaa);
    CHECK(parsed->fxaa_subpix == quality.fxaa_subpix);
    CHECK(parsed->fxaa_edge_threshold == quality.fxaa_edge_threshold);
    CHECK(parsed->fxaa_edge_threshold_min == quality.fxaa_edge_threshold_min);
    CHECK(parsed->fxaa_quality == quality.fxaa_quality);
    CHECK(parsed->enable_ao == quality.enable_ao);
    CHECK(parsed->ao_intensity == quality.ao_intensity);
    CHECK(parsed->ao_radius == quality.ao_radius);
    CHECK(parsed->ao_thickness == quality.ao_thickness);
    CHECK(parsed->ao_quality == quality.ao_quality);
    CHECK(parsed->ao_resolution_scale == quality.ao_resolution_scale);
    CHECK(parsed->enable_ao_denoise == quality.enable_ao_denoise);
    CHECK(parsed->debug_view == quality.debug_view);
    CHECK(parsed->overdraw_range == quality.overdraw_range);
    CHECK(parsed->exposure_stops == quality.exposure_stops);
    CHECK(parsed->tonemap_mode == quality.tonemap_mode);
    CHECK(parsed->contrast == quality.contrast);
    CHECK(parsed->saturation == quality.saturation);
    CHECK(parsed->enable_background == quality.enable_background);
}

TEST_CASE("invalid render quality state TOML is rejected") {
    CHECK_FALSE(renderQualityStateFromToml("[render").has_value());
}

TEST_CASE("startup overrides apply after persisted viewer config fields") {
    ViewerConfigState persisted;
    persisted.cull = CullOverride::ForceNoCull;
    persisted.auto_orbit = true;
    persisted.angle_cut = true;
    persisted.enable_pbr = false;

    Config cfg;
    cfg.cull = CullOverride::ForceCull;
    cfg.auto_orbit = false;
    cfg.angle_cut = false;
    cfg.enable_pbr = true;

    applyViewerConfigState(persisted, cfg, nullptr);
    ConfigStartupOverrides overrides;
    overrides.cull = CullOverride::ForceCull;
    overrides.enable_pbr = true;
    applyViewerStartupOverrides(overrides, cfg, nullptr);

    CHECK(cfg.cull == CullOverride::ForceCull);
    CHECK(cfg.auto_orbit == true);
    CHECK(cfg.angle_cut == true);
    CHECK(cfg.enable_pbr == true);
}

TEST_CASE("parseManifestViewSteer reads the archive [view] layer") {
    auto ov = parseManifestViewSteer(
        "[project]\nconfig = \"scene.toml\"\ngeometry = \"scene.nhb.zst\"\n\n"
        "[view]\nangle_cut = true\nangle_cut_start_deg = 15.0\nangle_cut_end_deg = 75.0\n"
        "enable_pbr = false\n\n"
        "[view.camera]\ntarget = [1.0, 2.0, 3.0]\ndistance = 20.0\nyaw_deg = 30.0\n"
        "pitch_deg = 10.0\nprojection = \"perspective\"\n");
    REQUIRE(ov.has_value());
    REQUIRE(ov->angle_cut.has_value());
    CHECK(*ov->angle_cut == true);
    REQUIRE(ov->angle_cut_start_deg.has_value());
    CHECK(*ov->angle_cut_start_deg == Catch::Approx(15.0));
    REQUIRE(ov->enable_pbr.has_value());
    CHECK(*ov->enable_pbr == false);
    REQUIRE(ov->camera.has_value());
    CHECK(ov->camera->distance == Catch::Approx(20.0));
    // Keys not present are left unset (so the URL/sidecar layers win).
    CHECK_FALSE(ov->auto_orbit.has_value());
    CHECK_FALSE(ov->cull.has_value());
}

TEST_CASE("parseManifestViewSteer returns nullopt without a [view] table") {
    CHECK_FALSE(
        parseManifestViewSteer("[project]\nconfig = \"a\"\ngeometry = \"b\"\n").has_value());
    CHECK_FALSE(parseManifestViewSteer("not valid toml [[[").has_value());
}

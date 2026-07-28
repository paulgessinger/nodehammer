#include <nodehammer/viewer/app_state.hpp>

#include <glm/gtc/constants.hpp>
#include <toml++/toml.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>

namespace nodehammer::viewer {
namespace {

std::optional<float> finiteFloat(const toml::table &tbl, std::string_view key) {
    const auto *value = tbl.get(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    std::optional<double> parsed = value->value<double>();
    if (!parsed) {
        if (auto i = value->value<int64_t>()) {
            parsed = static_cast<double>(*i);
        }
    }
    if (!parsed || !std::isfinite(*parsed)) {
        return std::nullopt;
    }
    return static_cast<float>(*parsed);
}

void readBool(const toml::table &tbl, std::string_view key, bool &out) {
    if (auto value = tbl[key].value<bool>()) {
        out = *value;
    }
}

void readFloat(const toml::table &tbl, std::string_view key, float &out) {
    if (auto value = finiteFloat(tbl, key)) {
        out = *value;
    }
}

template <typename Enum>
void readEnumIndex(const toml::table &tbl, std::string_view key, Enum &out, int min_idx,
                   int max_idx) {
    if (auto idx = tbl[key].value<int64_t>()) {
        if (*idx >= min_idx && *idx <= max_idx) {
            out = static_cast<Enum>(*idx);
        }
    }
}

toml::array vec3ToArray(const glm::vec3 &v) { return toml::array{v.x, v.y, v.z}; }

const char *projectionModeName(ProjectionMode mode) {
    switch (mode) {
    case ProjectionMode::Perspective:
        return "perspective";
    case ProjectionMode::Orthographic:
        return "orthographic";
    }
    return "perspective";
}

void readProjectionMode(const toml::table &tbl, std::string_view key, ProjectionMode &out) {
    if (auto value = tbl[key].value<std::string>()) {
        if (*value == "orthographic" || *value == "ortho") {
            out = ProjectionMode::Orthographic;
        } else if (*value == "perspective") {
            out = ProjectionMode::Perspective;
        }
    }
}

std::optional<glm::vec3> parseVec3(const toml::array &arr) {
    if (arr.size() != 3) {
        return std::nullopt;
    }
    std::array<float, 3> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto *node = arr.get(i);
        if (node == nullptr) {
            return std::nullopt;
        }
        std::optional<double> parsed = node->value<double>();
        if (!parsed) {
            if (auto integer = node->value<int64_t>()) {
                parsed = static_cast<double>(*integer);
            }
        }
        if (!parsed || !std::isfinite(*parsed)) {
            return std::nullopt;
        }
        values[i] = static_cast<float>(*parsed);
    }
    return glm::vec3{values[0], values[1], values[2]};
}

toml::table cameraToTable(const Camera &camera) {
    return toml::table{
        {"target", vec3ToArray(camera.target)},
        {"distance", camera.distance},
        {"yaw_deg", glm::degrees(camera.yaw)},
        {"pitch_deg", glm::degrees(camera.pitch)},
        {"projection", projectionModeName(camera.projection)},
        {"fov_deg", camera.fov_deg},
        {"near_plane", camera.near_plane},
        {"far_plane", camera.far_plane},
    };
}

std::optional<Camera> parseCamera(const toml::table &tbl) {
    Camera camera;
    const auto *target = tbl["target"].as_array();
    if (target == nullptr) {
        return std::nullopt;
    }
    if (auto parsed = parseVec3(*target)) {
        camera.target = *parsed;
    } else {
        return std::nullopt;
    }
    if (auto value = finiteFloat(tbl, "distance")) {
        camera.distance = *value;
    } else {
        return std::nullopt;
    }
    if (auto value = finiteFloat(tbl, "yaw_deg")) {
        camera.yaw = glm::radians(*value);
    } else {
        return std::nullopt;
    }
    if (auto value = finiteFloat(tbl, "pitch_deg")) {
        camera.pitch = glm::radians(*value);
    } else {
        return std::nullopt;
    }
    readProjectionMode(tbl, "projection", camera.projection);
    readFloat(tbl, "fov_deg", camera.fov_deg);
    readFloat(tbl, "near_plane", camera.near_plane);
    readFloat(tbl, "far_plane", camera.far_plane);
    camera.sanitize();
    return camera;
}

const char *cullOverrideName(CullOverride mode) {
    switch (mode) {
    case CullOverride::Auto:
        return "auto";
    case CullOverride::ForceCull:
        return "force-on";
    case CullOverride::ForceNoCull:
        return "force-off";
    }
    return "auto";
}

std::optional<CullOverride> parseCullOverride(std::string_view value) {
    if (value == "auto") {
        return CullOverride::Auto;
    }
    if (value == "force-on" || value == "on" || value == "true" || value == "1") {
        return CullOverride::ForceCull;
    }
    if (value == "force-off" || value == "off" || value == "false" || value == "0") {
        return CullOverride::ForceNoCull;
    }
    return std::nullopt;
}

void readCullOverride(const toml::table &tbl, std::string_view key, CullOverride &out) {
    if (auto value = tbl[key].value<std::string>()) {
        if (auto parsed = parseCullOverride(*value)) {
            out = *parsed;
        }
    }
}

} // namespace

ViewerConfigState viewerConfigStateFrom(const Config &cfg, const Camera &camera) {
    ViewerConfigState state;
    state.cull = cfg.cull;
    state.pause_when_unfocused = cfg.pause_when_unfocused;
    state.auto_orbit = cfg.auto_orbit;
    state.auto_orbit_speed_deg = cfg.auto_orbit_speed_deg;
    state.angle_cut = cfg.angle_cut;
    state.shader_angle_cut = cfg.shader_angle_cut;
    state.boolean_cut = cfg.boolean_cut;
    state.angle_cut_start_deg = cfg.angle_cut_start_deg;
    state.angle_cut_end_deg = cfg.angle_cut_end_deg;
    state.enable_pbr = cfg.enable_pbr;
    state.camera = camera;
    return state;
}

void applyViewerConfigState(const ViewerConfigState &state, Config &cfg, Camera *camera) {
    cfg.cull = state.cull;
    cfg.pause_when_unfocused = state.pause_when_unfocused;
    cfg.auto_orbit = state.auto_orbit;
    cfg.auto_orbit_speed_deg = state.auto_orbit_speed_deg;
    cfg.angle_cut = state.angle_cut;
    cfg.shader_angle_cut = state.shader_angle_cut;
    cfg.boolean_cut = state.boolean_cut;
    cfg.angle_cut_start_deg = state.angle_cut_start_deg;
    cfg.angle_cut_end_deg = state.angle_cut_end_deg;
    cfg.enable_pbr = state.enable_pbr;
    if (camera != nullptr && state.camera.has_value()) {
        *camera = *state.camera;
    }
}

void applyViewerStartupOverrides(const ConfigStartupOverrides &overrides, Config &cfg,
                                 Camera *camera) {
    if (overrides.cull) {
        cfg.cull = *overrides.cull;
    }
    if (overrides.pause_when_unfocused) {
        cfg.pause_when_unfocused = *overrides.pause_when_unfocused;
    }
    if (overrides.auto_orbit) {
        cfg.auto_orbit = *overrides.auto_orbit;
    }
    if (overrides.auto_orbit_speed_deg) {
        cfg.auto_orbit_speed_deg = *overrides.auto_orbit_speed_deg;
    }
    if (overrides.angle_cut) {
        cfg.angle_cut = *overrides.angle_cut;
    }
    if (overrides.shader_angle_cut) {
        cfg.shader_angle_cut = *overrides.shader_angle_cut;
    }
    if (overrides.boolean_cut) {
        cfg.boolean_cut = *overrides.boolean_cut;
    }
    if (overrides.angle_cut_start_deg) {
        cfg.angle_cut_start_deg = *overrides.angle_cut_start_deg;
    }
    if (overrides.angle_cut_end_deg) {
        cfg.angle_cut_end_deg = *overrides.angle_cut_end_deg;
    }
    if (overrides.enable_pbr) {
        cfg.enable_pbr = *overrides.enable_pbr;
    }
    if (overrides.camera) {
        if (camera != nullptr) {
            *camera = *overrides.camera;
        }
        cfg.initial_camera = *overrides.camera;
    }
}

std::optional<ConfigStartupOverrides> parseManifestViewSteer(std::string_view toml_bytes) {
    toml::table root;
    try {
        root = toml::parse(toml_bytes);
    } catch (const toml::parse_error &) {
        return std::nullopt;
    }
    const auto *view = root["view"].as_table();
    if (view == nullptr) {
        return std::nullopt;
    }
    ConfigStartupOverrides ov;
    if (auto v = (*view)["cull"].value<std::string>()) {
        if (auto parsed = parseCullOverride(*v)) {
            ov.cull = *parsed;
        }
    }
    if (auto v = (*view)["pause_when_unfocused"].value<bool>()) {
        ov.pause_when_unfocused = *v;
    }
    if (auto v = (*view)["auto_orbit"].value<bool>()) {
        ov.auto_orbit = *v;
    }
    if (auto v = finiteFloat(*view, "auto_orbit_speed_deg")) {
        ov.auto_orbit_speed_deg = *v;
    }
    if (auto v = (*view)["angle_cut"].value<bool>()) {
        ov.angle_cut = *v;
    }
    if (auto v = (*view)["shader_angle_cut"].value<bool>()) {
        ov.shader_angle_cut = *v;
    }
    if (auto v = (*view)["boolean_cut"].value<bool>()) {
        ov.boolean_cut = *v;
    }
    if (auto v = finiteFloat(*view, "angle_cut_start_deg")) {
        ov.angle_cut_start_deg = *v;
    }
    if (auto v = finiteFloat(*view, "angle_cut_end_deg")) {
        ov.angle_cut_end_deg = *v;
    }
    if (auto v = (*view)["enable_pbr"].value<bool>()) {
        ov.enable_pbr = *v;
    }
    if (const auto *cam = (*view)["camera"].as_table()) {
        ov.camera = parseCamera(*cam);
    }
    return ov;
}

std::string viewerConfigStateToToml(const ViewerConfigState &state) {
    toml::table ui{
        {"show_project", state.show_project},
        {"show_status", state.show_status},
        {"show_view", state.show_view},
        {"show_debug", state.show_debug},
        {"show_sync", state.show_sync},
        {"url_sync_continuous", state.url_sync_continuous},
        {"url_sync_camera", state.url_sync_camera},
        {"url_sync_view", state.url_sync_view},
    };
    toml::table view{
        {"cull", cullOverrideName(state.cull)},
        {"pause_when_unfocused", state.pause_when_unfocused},
        {"auto_orbit", state.auto_orbit},
        {"auto_orbit_speed_deg", state.auto_orbit_speed_deg},
        {"angle_cut", state.angle_cut},
        {"shader_angle_cut", state.shader_angle_cut},
        {"boolean_cut", state.boolean_cut},
        {"angle_cut_start_deg", state.angle_cut_start_deg},
        {"angle_cut_end_deg", state.angle_cut_end_deg},
        {"enable_pbr", state.enable_pbr},
    };
    toml::table root{{"version", 1}, {"ui", std::move(ui)}, {"view", std::move(view)}};
    if (state.camera.has_value()) {
        root.insert_or_assign("camera", cameraToTable(*state.camera));
    }
    if (!state.root_config_key.empty() || !state.root_geometry_key.empty()) {
        root.insert_or_assign("roots", toml::table{
                                           {"config", state.root_config_key},
                                           {"geometry", state.root_geometry_key},
                                       });
    }
    std::ostringstream out;
    out << root;
    return out.str();
}

std::optional<ViewerConfigState> viewerConfigStateFromToml(std::string_view bytes) {
    toml::table root;
    try {
        root = toml::parse(bytes);
    } catch (const toml::parse_error &) {
        return std::nullopt;
    }

    ViewerConfigState state;
    if (const auto *ui = root["ui"].as_table()) {
        readBool(*ui, "show_project", state.show_project);
        readBool(*ui, "show_status", state.show_status);
        readBool(*ui, "show_view", state.show_view);
        readBool(*ui, "show_debug", state.show_debug);
        readBool(*ui, "show_sync", state.show_sync);
        readBool(*ui, "url_sync_continuous", state.url_sync_continuous);
        readBool(*ui, "url_sync_camera", state.url_sync_camera);
        readBool(*ui, "url_sync_view", state.url_sync_view);
    }
    if (const auto *view = root["view"].as_table()) {
        readCullOverride(*view, "cull", state.cull);
        readBool(*view, "pause_when_unfocused", state.pause_when_unfocused);
        readBool(*view, "auto_orbit", state.auto_orbit);
        readFloat(*view, "auto_orbit_speed_deg", state.auto_orbit_speed_deg);
        readBool(*view, "angle_cut", state.angle_cut);
        readBool(*view, "shader_angle_cut", state.shader_angle_cut);
        readBool(*view, "boolean_cut", state.boolean_cut);
        readFloat(*view, "angle_cut_start_deg", state.angle_cut_start_deg);
        readFloat(*view, "angle_cut_end_deg", state.angle_cut_end_deg);
        readBool(*view, "enable_pbr", state.enable_pbr);
    }
    if (const auto *camera = root["camera"].as_table()) {
        state.camera = parseCamera(*camera);
    }
    if (const auto *roots = root["roots"].as_table()) {
        state.root_config_key = (*roots)["config"].value_or(std::string{});
        state.root_geometry_key = (*roots)["geometry"].value_or(std::string{});
    }
    return state;
}

std::string renderQualityStateToToml(const RenderQualitySettings &quality) {
    toml::table render{
        {"render_scale", quality.render_scale},
        {"dynamic_render_scale", quality.dynamic_render_scale},
        {"adaptive_render_scale", quality.adaptive_render_scale},
        {"render_scale_target_fps", quality.render_scale_target_fps},
        {"render_scale_min", quality.render_scale_min},
        {"render_scale_max", quality.render_scale_max},
        {"render_scale_memory_budget_mb", quality.render_scale_memory_budget_mb},
        {"cap_fps", quality.cap_fps},
        {"pause_when_static", quality.pause_when_static},
        {"enable_hdr", quality.enable_hdr},
        {"enable_tonemap", quality.enable_tonemap},
        {"enable_material_prefilter", quality.enable_material_prefilter},
        {"material_prefilter_scale", quality.material_prefilter_scale},
        {"material_prefilter_band", quality.material_prefilter_band},
        {"lod_hull_enable", quality.lod_hull_enable},
        {"lod_hull_force", quality.lod_hull_force},
        {"lod_hull_screen_px", quality.lod_hull_screen_px},
        {"lod_hull_band_px", quality.lod_hull_band_px},
        {"enable_fxaa", quality.enable_fxaa},
        {"fxaa_subpix", quality.fxaa_subpix},
        {"fxaa_edge_threshold", quality.fxaa_edge_threshold},
        {"fxaa_edge_threshold_min", quality.fxaa_edge_threshold_min},
        {"fxaa_quality", static_cast<int>(quality.fxaa_quality)},
        {"enable_ao", quality.enable_ao},
        {"ao_intensity", quality.ao_intensity},
        {"ao_radius", quality.ao_radius},
        {"ao_thickness", quality.ao_thickness},
        {"ao_quality", static_cast<int>(quality.ao_quality)},
        {"ao_resolution_scale", quality.ao_resolution_scale},
        {"enable_ao_denoise", quality.enable_ao_denoise},
        {"debug_view", static_cast<int>(quality.debug_view)},
        {"overdraw_range", quality.overdraw_range},
        {"exposure_stops", quality.exposure_stops},
        {"tonemap_mode", static_cast<int>(quality.tonemap_mode)},
        {"contrast", quality.contrast},
        {"saturation", quality.saturation},
        {"enable_background", quality.enable_background},
    };
    toml::table root{{"version", 1}, {"render", std::move(render)}};
    std::ostringstream out;
    out << root;
    return out.str();
}

std::optional<RenderQualitySettings> renderQualityStateFromToml(std::string_view bytes) {
    toml::table root;
    try {
        root = toml::parse(bytes);
    } catch (const toml::parse_error &) {
        return std::nullopt;
    }

    RenderQualitySettings quality;
    const auto *render = root["render"].as_table();
    if (render == nullptr) {
        return quality;
    }

    readFloat(*render, "render_scale", quality.render_scale);
    readBool(*render, "dynamic_render_scale", quality.dynamic_render_scale);
    readBool(*render, "adaptive_render_scale", quality.adaptive_render_scale);
    readFloat(*render, "render_scale_target_fps", quality.render_scale_target_fps);
    readFloat(*render, "render_scale_min", quality.render_scale_min);
    readFloat(*render, "render_scale_max", quality.render_scale_max);
    readFloat(*render, "render_scale_memory_budget_mb", quality.render_scale_memory_budget_mb);
    readBool(*render, "cap_fps", quality.cap_fps);
    readBool(*render, "pause_when_static", quality.pause_when_static);
    readBool(*render, "enable_hdr", quality.enable_hdr);
    readBool(*render, "enable_tonemap", quality.enable_tonemap);
    readBool(*render, "enable_material_prefilter", quality.enable_material_prefilter);
    readFloat(*render, "material_prefilter_scale", quality.material_prefilter_scale);
    readFloat(*render, "material_prefilter_band", quality.material_prefilter_band);
    readBool(*render, "lod_hull_enable", quality.lod_hull_enable);
    readBool(*render, "lod_hull_force", quality.lod_hull_force);
    readFloat(*render, "lod_hull_screen_px", quality.lod_hull_screen_px);
    readFloat(*render, "lod_hull_band_px", quality.lod_hull_band_px);
    readBool(*render, "enable_fxaa", quality.enable_fxaa);
    readFloat(*render, "fxaa_subpix", quality.fxaa_subpix);
    readFloat(*render, "fxaa_edge_threshold", quality.fxaa_edge_threshold);
    readFloat(*render, "fxaa_edge_threshold_min", quality.fxaa_edge_threshold_min);
    readEnumIndex(*render, "fxaa_quality", quality.fxaa_quality,
                  static_cast<int>(FxaaQualityPreset::Low),
                  static_cast<int>(FxaaQualityPreset::Ultra));
    readBool(*render, "enable_ao", quality.enable_ao);
    readFloat(*render, "ao_intensity", quality.ao_intensity);
    readFloat(*render, "ao_radius", quality.ao_radius);
    readFloat(*render, "ao_thickness", quality.ao_thickness);
    readEnumIndex(*render, "ao_quality", quality.ao_quality, static_cast<int>(AoQualityPreset::Low),
                  static_cast<int>(AoQualityPreset::Ultra));
    readFloat(*render, "ao_resolution_scale", quality.ao_resolution_scale);
    readBool(*render, "enable_ao_denoise", quality.enable_ao_denoise);
    readEnumIndex(*render, "debug_view", quality.debug_view, static_cast<int>(DebugView::Off),
                  static_cast<int>(DebugView::Overdraw));
    readFloat(*render, "overdraw_range", quality.overdraw_range);
    readFloat(*render, "exposure_stops", quality.exposure_stops);
    readEnumIndex(*render, "tonemap_mode", quality.tonemap_mode,
                  static_cast<int>(TonemapMode::ACES), static_cast<int>(TonemapMode::AgX));
    readFloat(*render, "contrast", quality.contrast);
    readFloat(*render, "saturation", quality.saturation);
    readBool(*render, "enable_background", quality.enable_background);
    return quality;
}

} // namespace nodehammer::viewer

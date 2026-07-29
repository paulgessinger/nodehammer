#pragma once

#include <viewer/camera.hpp>
#include <viewer/config.hpp>
#include <viewer/render_quality.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

struct ViewerConfigState {
    bool show_project{true};
    bool show_status{true};
    bool show_view{true};
    bool show_debug{true};
    bool show_sync{false};
    bool url_sync_continuous{false};
    bool url_sync_camera{true};
    bool url_sync_view{true};
    CullOverride cull{CullOverride::Auto};
    bool pause_when_unfocused{true};
    bool auto_orbit{false};
    float auto_orbit_speed_deg{15.f};
    bool angle_cut{false};
    bool shader_angle_cut{true};
    bool boolean_cut{false};
    float angle_cut_start_deg{0.f};
    float angle_cut_end_deg{90.f};
    bool enable_pbr{true};
    std::optional<Camera> camera;

    /// Active build roots the user selected by double-clicking in the Project
    /// panel. Persisted (web app mode) so a reload reproduces the selection;
    /// empty means "no explicit choice — fall back to the archive manifest".
    std::string root_config_key;
    std::string root_geometry_key;
};

[[nodiscard]] ViewerConfigState viewerConfigStateFrom(const Config &cfg, const Camera &camera);
void applyViewerConfigState(const ViewerConfigState &state, Config &cfg, Camera *camera);
void applyViewerStartupOverrides(const ConfigStartupOverrides &overrides, Config &cfg,
                                 Camera *camera);

/// Parse a project manifest's `[view]` table (and its `[view.camera]` sub-table)
/// into startup overrides — the *archive* layer of the steer cascade
/// (app default < archive [view] < sidecar < URL). Only keys present are set;
/// returns nullopt when there is no `[view]` table or the TOML is malformed.
[[nodiscard]] std::optional<ConfigStartupOverrides>
parseManifestViewSteer(std::string_view toml_bytes);

[[nodiscard]] std::string viewerConfigStateToToml(const ViewerConfigState &state);
[[nodiscard]] std::optional<ViewerConfigState> viewerConfigStateFromToml(std::string_view bytes);

[[nodiscard]] std::string renderQualityStateToToml(const RenderQualitySettings &quality);
[[nodiscard]] std::optional<RenderQualitySettings>
renderQualityStateFromToml(std::string_view bytes);

} // namespace nodehammer::viewer

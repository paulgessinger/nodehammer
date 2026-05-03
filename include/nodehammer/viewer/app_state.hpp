#pragma once

#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/config.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

struct ViewerConfigState {
    bool show_project{true};
    bool show_status{true};
    bool show_view{true};
    bool show_debug{true};
    bool cull_back{true};
    bool pause_when_unfocused{true};
    bool auto_orbit{false};
    float auto_orbit_speed_deg{15.f};
    bool angle_cut{false};
    bool shader_angle_cut{true};
    float angle_cut_start_deg{0.f};
    float angle_cut_end_deg{90.f};
    bool enable_pbr{true};
    std::optional<Camera> camera;
};

[[nodiscard]] ViewerConfigState viewerConfigStateFrom(const Config &cfg, const Camera &camera);
void applyViewerConfigState(const ViewerConfigState &state, Config &cfg, Camera *camera);
void applyViewerStartupOverrides(const ConfigStartupOverrides &overrides, Config &cfg,
                                 Camera *camera);

[[nodiscard]] std::string viewerConfigStateToToml(const ViewerConfigState &state);
[[nodiscard]] std::optional<ViewerConfigState> viewerConfigStateFromToml(std::string_view bytes);

} // namespace nodehammer::viewer

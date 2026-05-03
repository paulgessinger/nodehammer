#pragma once

#include <nodehammer/viewer/camera.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace nodehammer::viewer {

struct ConfigStartupOverrides {
    std::optional<bool> cull_back;
    std::optional<bool> pause_when_unfocused;
    std::optional<bool> auto_orbit;
    std::optional<float> auto_orbit_speed_deg;
    std::optional<bool> angle_cut;
    std::optional<bool> shader_angle_cut;
    std::optional<float> angle_cut_start_deg;
    std::optional<float> angle_cut_end_deg;
    std::optional<bool> enable_pbr;
    std::optional<Camera> camera;
};

struct Config {
    std::string title{"nodehammer viewer"};
    uint32_t width{1280};
    uint32_t height{720};
    bool vsync{true};
    bool cull_back{true};
    bool pause_when_unfocused{true};
    bool auto_orbit{false};
    float auto_orbit_speed_deg{15.f};
    bool angle_cut{false};
    bool shader_angle_cut{true};
    float angle_cut_start_deg{0.f};
    float angle_cut_end_deg{90.f};
    bool enable_pbr{true};
    std::optional<Camera> initial_camera;
    ConfigStartupOverrides startup_overrides;
};

} // namespace nodehammer::viewer

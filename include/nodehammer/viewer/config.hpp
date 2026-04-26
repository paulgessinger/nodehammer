#pragma once

#include <cstdint>
#include <string>

namespace nodehammer::viewer {

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
    bool enable_pbr{false};
};

} // namespace nodehammer::viewer

#pragma once

#include <viewer/camera.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace nodehammer::viewer {

/// Backface-cull override. `Auto` lets per-material `doubleSided` decide;
/// the two `Force*` variants are debug knobs that ignore the material flag.
enum class CullOverride : uint8_t {
    Auto = 0,
    ForceCull = 1,
    ForceNoCull = 2,
};

struct ConfigStartupOverrides {
    std::optional<CullOverride> cull;
    std::optional<bool> pause_when_unfocused;
    std::optional<bool> auto_orbit;
    std::optional<float> auto_orbit_speed_deg;
    std::optional<bool> angle_cut;
    std::optional<bool> shader_angle_cut;
    std::optional<bool> boolean_cut;
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
    CullOverride cull{CullOverride::Auto};
    bool pause_when_unfocused{true};
    bool auto_orbit{false};
    float auto_orbit_speed_deg{15.f};
    bool angle_cut{false};
    bool shader_angle_cut{true};
    /// Analytical Boolean wedge cut. Unlike the render-time shader/instance
    /// cut, this rebuilds the scene with watertight cut faces; toggling it or
    /// committing a new cut angle triggers an async re-tessellation.
    bool boolean_cut{false};
    float angle_cut_start_deg{0.f};
    float angle_cut_end_deg{90.f};
    bool enable_pbr{true};
    /// Embed mode (web): the viewer is hosted in an iframe by a page that owns
    /// the UI, the URL and all persistence. The App then draws only the 3D
    /// scene — no Dear ImGui panels, menus, toasts or drop hints (orbit / zoom /
    /// pan input still works) — never rewrites the browser URL, never reads or
    /// writes localStorage / IndexedDB, and talks to the host page over
    /// `window.postMessage` (camera pose + a one-shot ready signal). See
    /// `docs/viewer-embed.md`. Ignored on native.
    bool embed{false};
    std::optional<Camera> initial_camera;
    ConfigStartupOverrides startup_overrides;
};

} // namespace nodehammer::viewer

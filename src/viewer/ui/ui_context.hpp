#pragma once

#include <nodehammer/viewer/config.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/render_quality.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace nodehammer::viewer {

class BuildSession;
class ProjectFs;
class SceneBuildJob;
class SceneRenderer;
struct Camera;
struct IblSettings;

namespace ui {

class Notifications;

struct UiState {
    bool show_project{true};
    bool show_status{true};
    bool show_view{true};
    bool show_debug{true};
    bool dockspace_built{false};
};

struct UiActions {
    std::function<void()> sync_browser_url;
    std::function<void()> open_file_picker;
    std::function<void()> open_folder_picker;
    std::function<void()> frame_scene;
    std::function<void()> close_project;
    std::function<void()> rescan_project;
    std::function<void()> rebake_ibl;
    std::function<void(std::string)> select_config_key;
    std::function<void(std::string)> select_geometry_key;
};

struct ViewerUiContext {
    Config &cfg;
    RenderQualitySettings &quality;
    ProjectFs *project;
    BuildSession &build_session;
    SceneBuildJob &build_job;
    SceneRenderer &scene_renderer;
    Camera &camera;
    Notifications *notifications;
    const platform::PlatformWindowState &platform_window_state;

    std::string &root_config_key;
    std::string &root_geometry_key;
    std::string &build_error;

    std::uint32_t fb_width{0};
    std::uint32_t fb_height{0};
    float fps{0.f};
    double frame_interval_ms{0.0};
    double render_submit_ms{0.0};
    double scene_submit_ms{0.0};

    bool has_scene{false};
    bool scene_uploaded{false};
    bool build_in_progress{false};
    bool ibl_installed{false};
    IblSettings *ibl_settings{nullptr};
};

} // namespace ui
} // namespace nodehammer::viewer

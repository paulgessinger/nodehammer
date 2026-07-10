#pragma once

#include "../gpu_pass_timer.hpp"

#include <nodehammer/viewer/config.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/png_export.hpp>
#include <nodehammer/viewer/render_quality.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace nodehammer::viewer {

class BuildSession;
class ProjectFs;
class SceneBuildJob;
class SceneRenderer;
struct Camera;
struct IblSettings;

namespace ui {

class Notifications;
struct PerfHistory;

struct UiState {
    bool show_project{true};
    bool show_status{true};
    bool show_view{true};
    bool show_debug{true};
    bool dockspace_built{false};

    // Sync-to-URL window (web): which steer to write to the address bar and
    // whether to do it continuously. Persisted with the viewer config state.
    bool show_sync{false};
    bool url_sync_continuous{false};
    bool url_sync_camera{true};
    bool url_sync_view{true};
};

struct UiActions {
    std::function<void()> sync_browser_url;
    std::function<void(const std::string &)> open_url;
    std::function<void()> open_file_picker;
    std::function<void()> open_folder_picker;
    /// Open a `.nhproj` project as a live archive mode (cross-platform).
    std::function<void()> open_archive;
    /// Promote the current project into a live, unbound archive bundle seeded
    /// with its archivable content. Gated by `can_create_archive`.
    std::function<void()> create_archive_from_scene;
    /// Persist the current archive: write in place if bound, else save-as
    /// (native) / download (web). Gated by `is_archive_mode`.
    std::function<void()> save_archive;
    /// Publish the current project as a self-contained web package (web only).
    std::function<void()> publish_package;
    std::function<void()> frame_scene;
    std::function<void()> close_project;
    std::function<void()> rescan_project;
    std::function<void()> rebake_ibl;
    /// Request an async scene re-tessellation (e.g. the Boolean angle cut was
    /// toggled or its angle committed). Coalesced and serviced by the App's
    /// build-drive loop from the cached pristine scene.
    std::function<void()> request_scene_rebuild;
    /// Kick off a high-resolution PNG export using the current
    /// `ViewerUiContext::export_settings`. Coalesced — ignored while another
    /// export is already in flight.
    std::function<void()> export_png;
    std::function<void()> reset_render_quality;
    std::function<void(std::string)> select_config_key;
    std::function<void(std::string)> select_geometry_key;
    /// Request removal of a project file by key. The App consults the backend's
    /// planRemove and, on Confirm, shows a confirmation modal before committing.
    std::function<void(std::string)> remove_key;
};

/// Per-frame sokol draw-submission counters for the Debug panel. Plain ints so
/// this header (pulled into every UI TU) stays free of sokol_gfx.h; the App
/// copies the fields out of sg_frame_stats. `mtl_*` are Metal-backend specific
/// and 0 on other backends. set/skip vertex-buffer counts show how many binds
/// actually hit the driver vs. were deduplicated by sokol's state cache.
struct RenderCallStats {
    std::uint32_t num_apply_pipeline{0};
    std::uint32_t num_apply_bindings{0};
    std::uint32_t num_apply_uniforms{0};
    std::uint32_t num_draw{0};
    std::uint32_t mtl_set_render_pipeline_state{0};
    std::uint32_t mtl_set_vertex_buffer{0};
    std::uint32_t mtl_skip_vertex_buffer{0};
};

struct ViewerUiContext {
    Config &cfg;
    RenderQualitySettings &quality;
    PngExportSettings &export_settings;
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

    /// Set of project-tree directory keys the user has expanded. The Project
    /// panel drives each directory's open state from this set and writes toggles
    /// back into it; the App persists it across sessions via a custom ImGui .ini
    /// settings handler (ImGui itself does not serialize tree-node open state).
    std::unordered_set<std::string> *project_tree_open{nullptr};

    std::uint32_t fb_width{0};
    std::uint32_t fb_height{0};
    /// Actual offscreen render-target resolution (window size × render_scale).
    /// Differs from fb_width/height when render_scale != 1.
    std::uint32_t scene_width{0};
    std::uint32_t scene_height{0};
    /// Actual AO-pass resolution (scene size × ao_resolution_scale). 0 when AO
    /// is off.
    std::uint32_t ao_width{0};
    std::uint32_t ao_height{0};
    float fps{0.f};
    double frame_interval_ms{0.0};
    double render_submit_ms{0.0};
    double encode_ms{0.0};
    double present_ms{0.0};
    double gpu_wait_ms{0.0};
    double scene_submit_ms{0.0};
    RenderCallStats call_stats{};
    /// Rolling timing history for the Debug panel's live perf graphs. Owned by
    /// the App; null only in contexts that don't track frame timings.
    PerfHistory *perf_history{nullptr};
    /// Most recent completed frame's per-pass GPU times. `valid` is false on
    /// backends without timestamp support (everything but D3D11 today). Owned by
    /// the App; null in contexts that don't render.
    const GpuPassTimings *gpu_pass_times{nullptr};

    /// The active project is an `ArchiveProjectFs` (enables the archive Save
    /// action). Derived by the App from the current backend.
    bool is_archive_mode{false};
    /// The archive has unsaved working-set edits.
    bool archive_dirty{false};
    /// The archive has no bound filesystem path yet (created from a scene, not
    /// saved). Save becomes "Save archive as…" (native) / "Download" (web).
    bool archive_unbound{false};
    /// The current project can be promoted to an archive bundle (has a scene and
    /// isn't already in archive mode).
    bool can_create_archive{false};

    bool has_scene{false};
    bool scene_uploaded{false};
    bool build_in_progress{false};
    /// A high-resolution PNG export is currently rendering / reading back.
    bool export_in_progress{false};
    bool ibl_installed{false};
    bool hdr_supported{false};
    IblSettings *ibl_settings{nullptr};
};

} // namespace ui
} // namespace nodehammer::viewer

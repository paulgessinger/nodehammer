#include <nodehammer/viewer/app.hpp>

#include "ao_denoise_pass.hpp"
#include "ao_pass.hpp"
#include "ao_render_target.hpp"
#include "composite_pass.hpp"
#include "ibl.hpp"
#include "imgui_backend.hpp"
#include "png_export_readback.hpp"
#include "scene_build_job.hpp"
#include "scene_render_target.hpp"
#include "scene_renderer.hpp"
#include "ui/icon_font.hpp"
#include "ui/notifications.hpp"
#include "ui/perf_history.hpp"
#include "ui/viewer_ui.hpp"

#include <nodehammer/viewer/backend_caps.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/png_export.hpp>
#include <nodehammer/viewer/render_quality.hpp>

#include <nodehammer/ir/render.hpp>
#include <nodehammer/scene_build.hpp>
#include <nodehammer/viewer/app_state.hpp>
#include <nodehammer/viewer/build_session.hpp>
#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <imgui.h>
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>
#include <sokol_time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

enum class ScrollInputMode { Wheel, Trackpad };

struct RetainedModal {
    std::uint64_t id{0};
    std::string title;
    std::string message;
    std::string confirm_label{"OK"};
    std::string cancel_label{"Cancel"};
    bool show_cancel{false};
    bool opened{false};
    std::function<void()> on_confirm;
    std::function<void()> on_cancel;
};

bool isZoomModifier(uint32_t modifiers) {
    return (modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER)) != 0;
}

bool isPanModifier(uint32_t modifiers) { return (modifiers & SAPP_MODIFIER_SHIFT) != 0; }

std::string formatUrlFloat(float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    std::string s = out.str();
    while (s.size() > 1 && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

void appendUrlParam(std::string &query, std::string_view name, std::string_view value) {
    if (!query.empty()) {
        query.push_back('&');
    }
    query.append(name);
    query.push_back('=');
    query.append(value);
}

void appendUrlBool(std::string &query, std::string_view name, bool value, bool default_value) {
    if (value != default_value) {
        appendUrlParam(query, name, value ? "1" : "0");
    }
}

void appendUrlFloat(std::string &query, std::string_view name, float value, float default_value) {
    if (std::abs(value - default_value) >= 0.0001f) {
        appendUrlParam(query, name, formatUrlFloat(value));
    }
}

// Timestamped default name for an exported screenshot, e.g.
// "nodehammer-screenshot-20260524-153012.png".
std::string makeScreenshotFilename() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "nodehammer-screenshot-%Y%m%d-%H%M%S.png", &tm) == 0) {
        return "nodehammer-screenshot.png";
    }
    return std::string{buf};
}

} // namespace

constexpr const char *kViewerConfigStateKey = "viewer-state.toml";
constexpr const char *kImGuiStateKey = "imgui.ini";
constexpr double kPersistenceSaveIntervalSeconds = 1.0;
constexpr std::chrono::milliseconds kIblRebakeDebounce{300};

struct App::Impl {
    Config cfg;
    bool launch_had_initial_camera{false};
    bool quit{false};

    // The viewer keeps two scenes GPU-resident at once so the angle cut can flip
    // instantly between an interactive shader preview and the baked Boolean cut
    // without re-uploading: `scene` is the uncut base (shown with the render-time
    // shader cut while dragging), `cut_scene` is the watertight Boolean-cut bake
    // (shown once the angle settles). They never apply both cuts at the same time
    // (that z-fights the shader's discard plane against the Boolean cut faces).
    std::shared_ptr<const RenderScene> scene;
    std::shared_ptr<const RenderScene> cut_scene;
    std::unique_ptr<ProjectFs> project_;
    /// Platform impl: native vs web. Constructed by App's ctor body
    /// after this Impl is in place, so the impl can hold a back-pointer
    /// to the live App. State that would otherwise live in file-static
    /// globals (picker latches, window hooks) lives as platform members.
    std::unique_ptr<platform::Platform> platform_;
    platform::WindowCustomizationRequest window_customization;
    platform::PlatformWindowState platform_window_state;
    std::vector<platform::PlatformGestureEvent> platform_gesture_events;
    SceneRenderer scene_renderer;     ///< draws the uncut base scene
    SceneRenderer cut_renderer;       ///< draws the Boolean-cut scene
    SceneRenderer *active_renderer{}; ///< last renderer drawn; for UI stats
    SceneRenderTarget scene_rt;
    // AO targets, all sized to ao_resolution_scale of the *scaled* scene
    // resolution (coupled to the depth buffer the GTAO pass reads — computing AO
    // at a different resolution than its source depth quantizes it to the
    // depth-texel grid and looks blocky).
    //
    // `ao_rt_raw` is the GTAO output (transient; only allocated when denoise is
    // on, since denoise reads it and writes a history buffer). The final AO
    // (denoised, or raw when denoise is off) lands in a ping-pong pair of
    // history buffers `ao_hist`. The scene pass is frame-late: it samples last
    // frame's result (`ao_hist[ao_hist_last_written]`) while the GTAO/denoise
    // passes write the *other* buffer. That double-buffering is what lets a
    // dynamic-scale step resize only the write buffer — the read buffer the
    // scene is sampling keeps its previous-resolution contents (sampling is
    // normalized-UV, so the size mismatch is invisible), so a scale step no
    // longer drops AO for a frame (the flicker we used to get from reallocating
    // a single shared history target). `ao_history_valid` gates the sample until
    // the first AO write exists.
    AoRenderTarget ao_rt_raw;
    AoRenderTarget ao_hist[2];
    int ao_hist_last_written{0};
    bool ao_history_valid{false};
    AoPass ao_pass;
    AoDenoisePass ao_denoise_pass;
    CompositePass composite;
    RenderQualitySettings quality;
    Camera camera;

    // ── PNG screenshot export ────────────────────────────────────────────────
    // High-res export: render the scene at the export resolution with every
    // quality knob maxed, composite into a dedicated LDR target, read it back,
    // box-downscale to the requested output size, then write (native) or
    // download (web) a PNG. Driven as a small state machine across frames from
    // onFrame because the readback is async on some backends — and even on Metal
    // we wait for the capture frame's GPU work to drain. The live scene/AO
    // targets are reused (resized to the export resolution); only the composite
    // output target `export_out_rt` is export-specific.
    enum class ExportPhase {
        Idle,
        Rendering, // rendering export-res frames so the AO temporal denoise converges
        WaitGpu,   // capture composite issued; waiting for its GPU work to finish
        Readback   // reading back, then downscale + encode + deliver
    };
    ExportPhase export_phase{ExportPhase::Idle};
    PngExportSettings export_settings;    // edited by the UI; snapshot taken at request time
    RenderQualitySettings export_quality; // maxed snapshot used while exporting
    SceneRenderTarget export_out_rt;      // composite output (LDR, swapchain format)
    ImageReadback export_readback;
    uint32_t export_internal_w{0};
    uint32_t export_internal_h{0};
    int export_converge_count{0};
    int export_wait_count{0};
    bool export_render_active{false}; // render() targets the export resolution this frame
    bool export_capture{false};       // render() also composites into export_out_rt this frame
    bool export_readback_started{false};
    ui::Notifications::ProgressHandle export_progress{0};
    std::string export_filename;
    // When set, the encoded PNG is written straight to this path instead of
    // going through the platform delivery (cwd file / browser download). Used by
    // the headless --screenshot CLI mode. `export_quit_when_done` makes the app
    // quit once the export resolves (success or failure).
    std::string export_explicit_path;
    bool export_quit_when_done{false};
    // Pending one-shot startup screenshot (set by App::requestScreenshot before
    // run()). Triggered from onFrame once the scene is loaded and settled.
    bool startup_screenshot_pending{false};
    std::string startup_screenshot_path;
    PngExportSettings startup_screenshot_settings;

    // Procedural IBL bake. Runs on the GPU in a single frame the first
    // time `onFrame` ticks; until then `scene_renderer` samples from 1×1
    // dummy textures created by `IblResources::createDummy()`. The user
    // can edit `ibl_settings` and click "Rebake IBL" — the action sets
    // `ibl_rebake_pending` and onFrame consumes it on the next tick (a
    // bake pass cannot be issued from inside the swapchain pass that
    // hosts the UI draw).
    bool ibl_installed{false};
    bool ibl_rebake_pending{false};
    IblSettings ibl_settings{};
    // Debounced auto-rebake: while the user drags an IBL slider, every frame
    // observes a different value and pushes `ibl_settle_at` forward; the
    // bake is only triggered after the settings have been stable for
    // `kIblRebakeDebounce`.
    IblSettings ibl_last_seen_settings{};
    IblSettings ibl_last_baked_settings{};
    std::chrono::steady_clock::time_point ibl_settle_at{};

    // Off-loop scene tessellation. Native runs the build on a worker
    // thread so the UI stays smooth. Web defers the synchronous build by
    // one frame so the previous frame paints a "Tessellating…" message
    // before the page freezes.
    SceneBuildJob build_job;
    bool build_in_progress{false};
    std::chrono::steady_clock::time_point build_start_time{};

    // Pristine (pre-cut) build inputs, cached on each fresh BuildSession build
    // so toggling or re-aiming the Boolean angle cut can re-prep + re-tessellate
    // from uncut geometry without re-walking the project. The cut is applied in
    // the prep stage, which mutates the scene, so we must always re-derive from
    // this clean copy rather than from the already-cut result.
    std::shared_ptr<const ::nodehammer::SemanticScene> pristine_scene;
    std::shared_ptr<const ::nodehammer::NHConfig> pristine_config;
    std::string pristine_config_label;
    std::string pristine_geometry_label;

    // Boolean-cut build state. The base build runs once per project load; the
    // cut build runs on demand (toggle/commit) from the pristine scene.
    bool cut_uploaded{false};
    bool building_cut{false};        ///< the in-flight build is a cut (vs base) build
    bool pending_cut_rebuild{false}; ///< a cut (re)build was requested
    bool last_shown_cut{false};      ///< whether last frame drew the cut scene (AO flip reset)
    float cut_built_start_deg{0.f};  ///< angle the resident cut scene was built at
    float cut_built_end_deg{0.f};
    float in_flight_cut_start_deg{0.f}; ///< angle the in-flight cut build is using
    float in_flight_cut_end_deg{0.f};

    // Live progress-toast handle for the build. 0 means "no toast in flight";
    // populated when we kick off the build and cleared on finish/cancel.
    ui::Notifications::ProgressHandle build_progress_handle{0};

    // BuildSession drives the include-graph walk against the project's
    // resolve() interface and produces parsed config + imported geometry
    // for the build job. App owns it so the frame loop can poll once
    // per frame.
    BuildSession build_session;

    // Root keys the App last fed to the session. Set by external
    // entry points via App::setRootKeys (URL JS shell, CLI) and by
    // double-click in the tree panel. The user can override the
    // initial selection at any time by clicking a different leaf.
    std::string root_config_key;
    std::string root_geometry_key;

    // Stashed message after a build failure so the UI can keep showing
    // it across frames.
    std::string build_error;

    std::vector<RetainedModal> active_modals;
    std::uint64_t next_modal_id{1};
    ui::UiState ui_state;
    ui::Notifications notifications;
    std::string last_saved_viewer_config_state;
    std::string last_saved_imgui_state;
    uint64_t last_persistence_save_time{0};
    ConfigStartupOverrides startup_overrides;

    /// Bounding-sphere radius of the loaded scene; live-only state derived
    /// from the scene geometry, not part of persisted camera state. 0 means
    /// "no scene framed yet" — `dolly` falls back to distance-relative sizing.
    float scene_radius{0.f};
    bool scene_uploaded{false};
    bool camera_framed{false};

    bool window_focused{true};
    bool window_visible{true};

    uint32_t fb_width{0};
    uint32_t fb_height{0};

    uint64_t last_time{0};
    double delta_seconds{0.0};

    // Dynamic render-scale state. When quality.dynamic_render_scale is on, the
    // offscreen scale drops while the camera moves (to protect framerate) and
    // jumps back up to render_scale_max once it settles. dyn_scale is the value
    // applied this frame. With adaptive scaling on, dyn_motion_scale is the
    // closed-loop in-motion scale driven by frame timing (dyn_frame_ms_ema is
    // its smoothed input; dyn_climb_lock pauses upward probing after an
    // overshoot, and dyn_last_scale_change rate-limits how often the applied
    // scale changes at all — both directions — so orbiting doesn't reallocate
    // the offscreen targets every few frames). The dyn_prev_* fields snapshot
    // last frame's camera to detect motion.
    float dyn_scale{1.0f};
    float dyn_motion_scale{0.5f};
    double dyn_frame_ms_ema{0.0};
    bool dyn_was_moving{false};
    uint64_t dyn_settle_anchor{0};
    uint64_t dyn_climb_lock{0};
    uint64_t dyn_last_scale_change{0};
    glm::vec3 dyn_prev_target{0.f};
    float dyn_prev_yaw{0.f};
    float dyn_prev_pitch{0.f};
    float dyn_prev_distance{0.f};
    float dyn_prev_fov{0.f};
    ProjectionMode dyn_prev_proj{ProjectionMode::Perspective};

    // Sokol-time stamp of the last user-input-class event. Drives the
    // idle-throttle decision: a recent input keeps us at full vsync
    // rate even when the OS reports the window as backgrounded, so
    // scroll-wheel control and the drag overlay stay reactive. 0 means
    // "no input observed yet" — treated as idle.
    uint64_t last_activity_time{0};

    // Decoupled on-demand scene rendering: the cheap composite + ImGui run on
    // every awake frame (so the UI stays fully live — hover, panels, graphs),
    // but the expensive scene + AO passes only re-run when the scene actually
    // changes. last_scene_change timestamps the last scene-affecting change
    // (camera, a held widget, a job, IBL); we keep re-rendering the scene for a
    // settle window afterward so the AO temporal denoise and the dynamic-scale
    // settle jump converge. last_scene_consumed_ao caches the scene pass's AO
    // decision so compositing a *cached* scene still gates the AO multiply
    // correctly. Both only matter while pause_when_static is on.
    uint64_t last_scene_change{0};
    bool last_scene_consumed_ao{false};

    uint64_t frame_count{0};
    uint64_t fps_window_start{0};
    float fps{0.f};
    double frame_interval_ms{0.0};
    double render_submit_ms{0.0};
    // Breakdown of render_submit_ms (the "CPU submit" total): `encode_ms` is the
    // CPU time spent encoding the offscreen passes (scene + AO + denoise);
    // `present_ms` covers the swapchain pass — drawable acquisition + composite +
    // ImGui + sg_commit. On Metal the drawable acquisition (sglue_swapchain)
    // blocks on GPU backpressure when the GPU is behind, so present_ms >>
    // encode_ms means the GPU, not CPU submission, is the bottleneck.
    double encode_ms{0.0};
    double present_ms{0.0};
    // Subset of encode_ms: the frame's first sg_begin_pass, which blocks on the
    // in-flight-frames semaphore when the GPU is behind. This is GPU backpressure
    // masquerading as encode time; encode_ms - gpu_wait_ms is the true CPU encode.
    double gpu_wait_ms{0.0};
    double scene_submit_ms{0.0};
    // sokol per-frame draw-submission counters, snapshotted after sg_commit().
    // Surfaced in the Debug panel to attribute the CPU encode cost (pipeline
    // switches, bind/uniform/draw counts, redundant-bind skips).
    sg_frame_stats render_stats{};
    // Rolling per-frame timing history feeding the Debug panel's live graphs.
    ui::PerfHistory perf_history;

    ScrollInputMode scroll_input_mode{ScrollInputMode::Wheel};
    float pending_scroll_x{0.f};
    float pending_scroll_y{0.f};
    uint32_t pending_scroll_modifiers{0};
    uint64_t last_scroll_time{0};
    int smooth_scroll_score{0};
    int wheel_scroll_score{0};
    bool scroll_seq_pan{false};      // gesture mode at sequence start: Shift (pan) held
    bool scroll_seq_zoom{false};     // gesture mode at sequence start: Ctrl/Super (zoom) held
    bool scroll_seq_canceled{false}; // a modifier changed mid-sequence — swallow the kinetic tail

    explicit Impl(Config c)
        : cfg(std::move(c)), launch_had_initial_camera(cfg.initial_camera.has_value()),
          project_(platform::makeEmptyBag()), startup_overrides(std::move(cfg.startup_overrides)) {}

    void onInit();
    void onFrame();
    void onEvent(const sapp_event *ev);
    void onCleanup();
    void loadViewerConfigState();
    void applyStartupOverrides();
    void loadImGuiState();
    [[nodiscard]] std::string currentViewerConfigStateToml() const;
    void savePersistentState(bool force);

    void classifyScroll(float scroll_x, float scroll_y);
    void handleScrollEvent(const sapp_event *ev, bool imgui_handled);
    [[nodiscard]] bool shouldThrottleIdle() const;
    void updateCameraInput();
    void applyInitialCamera();
    void render();
    void ensureSceneTarget(uint32_t width, uint32_t height);
    void requestPngExport(const PngExportSettings &settings, std::string explicit_path = {},
                          bool quit_when_done = false);
    void exportPreRender();
    void exportPostRender();
    [[nodiscard]] std::optional<std::string> deliverExport(const std::vector<std::uint8_t> &png);
    void finishExport(bool ok, const std::string &message);
    void syncBrowserUrl() const;
    void addProjectPath(const std::filesystem::path &path);
    void addProjectBytes(const std::string &filename, std::span<const std::byte> bytes);
    void enqueueModal(RetainedModal modal);
    void enqueueProjectDropModal(ProjectDropDecision decision, std::function<void()> on_confirm);
    void renderActiveModal();
    [[nodiscard]] std::string browserUrlStateQuery() const;

    static void initCb(void *user) { static_cast<Impl *>(user)->onInit(); }
    static void frameCb(void *user) { static_cast<Impl *>(user)->onFrame(); }
    static void eventCb(const sapp_event *ev, void *user) {
        static_cast<Impl *>(user)->onEvent(ev);
    }
    static void cleanupCb(void *user) { static_cast<Impl *>(user)->onCleanup(); }
};

void App::Impl::loadViewerConfigState() {
    auto bytes = platform_->loadPersistentText(kViewerConfigStateKey);
    if (!bytes || bytes->empty()) {
        return;
    }
    auto state = viewerConfigStateFromToml(*bytes);
    if (!state) {
        std::println(stderr, "viewer: ignoring invalid persisted viewer state");
        return;
    }
    ui_state.show_project = state->show_project;
    ui_state.show_status = state->show_status;
    ui_state.show_view = state->show_view;
    ui_state.show_debug = state->show_debug;

    const bool keep_launch_camera = launch_had_initial_camera;
    applyViewerConfigState(*state, cfg, keep_launch_camera ? nullptr : &camera);
    if (!keep_launch_camera && state->camera.has_value()) {
        cfg.initial_camera = *state->camera;
    }
    last_saved_viewer_config_state = currentViewerConfigStateToml();
}

void App::Impl::applyStartupOverrides() {
    applyViewerStartupOverrides(startup_overrides, cfg, &camera);
    last_saved_viewer_config_state = currentViewerConfigStateToml();
}

void App::Impl::loadImGuiState() {
    auto bytes = platform_->loadPersistentText(kImGuiStateKey);
    if (!bytes || bytes->empty()) {
        return;
    }
    ImGui::LoadIniSettingsFromMemory(bytes->data(), bytes->size());
    last_saved_imgui_state = *bytes;
    ui_state.dockspace_built = true;
}

std::string App::Impl::currentViewerConfigStateToml() const {
    auto state = viewerConfigStateFrom(cfg, camera);
    state.show_project = ui_state.show_project;
    state.show_status = ui_state.show_status;
    state.show_view = ui_state.show_view;
    state.show_debug = ui_state.show_debug;
    return viewerConfigStateToToml(state);
}

void App::Impl::savePersistentState(bool force) {
    const uint64_t now = stm_now();
    if (!force && last_persistence_save_time != 0 &&
        stm_sec(stm_diff(now, last_persistence_save_time)) < kPersistenceSaveIntervalSeconds) {
        return;
    }

    const std::string viewer_state = currentViewerConfigStateToml();
    if (force || viewer_state != last_saved_viewer_config_state) {
        platform_->savePersistentText(kViewerConfigStateKey, viewer_state);
        last_saved_viewer_config_state = viewer_state;
    }

    ImGuiIO &io = ImGui::GetIO();
    if (force || io.WantSaveIniSettings) {
        std::size_t size = 0;
        const char *data = ImGui::SaveIniSettingsToMemory(&size);
        std::string imgui_state{data, size};
        if (force || imgui_state != last_saved_imgui_state) {
            platform_->savePersistentText(kImGuiStateKey, imgui_state);
            last_saved_imgui_state = std::move(imgui_state);
        }
        io.WantSaveIniSettings = false;
    }

    last_persistence_save_time = now;
}

void App::Impl::onInit() {
    loadViewerConfigState();
    applyStartupOverrides();

    sg_desc gfx_desc{};
    gfx_desc.environment = sglue_environment();
    gfx_desc.logger.func = slog_func;
    // Default sokol pool sizes (128 buffers / 128 images / 64 pipelines /
    // 16 passes) are tuned for sample programs. ODD scenes carry ~1k mesh
    // assets, each consuming a vbuf + an ibuf — bump generously so we
    // don't have to retune per dataset.
    gfx_desc.buffer_pool_size = 8192;
    gfx_desc.image_pool_size = 1024;
    gfx_desc.shader_pool_size = 256;
    gfx_desc.pipeline_pool_size = 256;
    sg_setup(&gfx_desc);
    // Per-frame draw-submission counters (apply_pipeline/bindings/uniforms/draw
    // + backend bind set-vs-skip), surfaced in the Debug panel to diagnose CPU
    // submit cost. Cheap — just increments while encoding.
    sg_enable_stats();

    // The renderer initialises with 1×1 placeholder IBL textures so the
    // first frame can draw before the GPU bake runs; onFrame swaps in the
    // real result on the first tick.
    scene_renderer.initialize();
    cut_renderer.initialize();
    active_renderer = &scene_renderer;
    ao_pass.initialize();
    ao_denoise_pass.initialize();
    composite.initialize();

    stm_setup();
    last_time = stm_now();

    IMGUI_CHECKVERSION();
    // simgui_setup creates the ImGui context, applies the dark style, and
    // sets ini_filename internally — DO NOT call ImGui::CreateContext or
    // ImGui::StyleColorsDark here, that would double-init and crash on
    // simgui_shutdown.
    ImGui_ImplSokol_Init();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    loadImGuiState();
    ui::icon_font::initialize();
    project_->setLogSink(&notifications);
    build_session.setLogSink(&notifications);

    fb_width = static_cast<uint32_t>(sapp_width());
    fb_height = static_cast<uint32_t>(sapp_height());
    platform_->attachWindow(window_customization);
}

void App::Impl::onEvent(const sapp_event *ev) {
    platform_->handleWindowEvent(ev);
    const bool imgui_handled = ImGui_ImplSokol_HandleEvent(ev);
    if (ev->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        handleScrollEvent(ev, imgui_handled);
    }
    if (ev->type == SAPP_EVENTTYPE_FOCUSED) {
        window_focused = true;
        last_time = stm_now();
        fps_window_start = last_time;
        frame_count = 0;
    } else if (ev->type == SAPP_EVENTTYPE_UNFOCUSED) {
        window_focused = false;
    } else if (ev->type == SAPP_EVENTTYPE_ICONIFIED || ev->type == SAPP_EVENTTYPE_SUSPENDED) {
        window_visible = false;
    } else if (ev->type == SAPP_EVENTTYPE_RESTORED || ev->type == SAPP_EVENTTYPE_RESUMED) {
        window_visible = true;
        last_time = stm_now();
        fps_window_start = last_time;
        frame_count = 0;
    } else if (ev->type == SAPP_EVENTTYPE_QUIT_REQUESTED) {
        savePersistentState(true);
        quit = true;
    } else if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        // Platform code pushes the dropped files into the App's existing
        // project (synchronously on native, via per-file fetch callbacks
        // on web). Lives in platform-specific code.
        platform_->dispatchDroppedFiles();
    }

    // Anything that looks like the user poking at the window bumps the
    // activity clock — the frame loop reads this to stay at full rate
    // while interactive, even when another window owns focus.
    switch (ev->type) {
    case SAPP_EVENTTYPE_MOUSE_MOVE:
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP:
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
    case SAPP_EVENTTYPE_MOUSE_ENTER:
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
    case SAPP_EVENTTYPE_KEY_DOWN:
    case SAPP_EVENTTYPE_KEY_UP:
    case SAPP_EVENTTYPE_CHAR:
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
    case SAPP_EVENTTYPE_FILES_DROPPED:
    case SAPP_EVENTTYPE_FOCUSED:
    case SAPP_EVENTTYPE_RESTORED:
    case SAPP_EVENTTYPE_RESUMED:
    case SAPP_EVENTTYPE_RESIZED:
        last_activity_time = stm_now();
        break;
    default:
        break;
    }
}

void App::Impl::classifyScroll(float scroll_x, float scroll_y) {
    constexpr double kQuietSeconds = 0.25;
    constexpr float kScrollEpsilon = 0.001f;
    constexpr float kStepEpsilon = 0.025f;
    constexpr int kTrackpadScoreThreshold = 2;
    constexpr int kWheelScoreThreshold = 2;

    const uint64_t now = stm_now();
    if (last_scroll_time != 0 && stm_sec(stm_diff(now, last_scroll_time)) > kQuietSeconds) {
        smooth_scroll_score = 0;
        wheel_scroll_score = 0;
    }
    last_scroll_time = now;

    const float abs_x = std::abs(scroll_x);
    const float abs_y = std::abs(scroll_y);
    const bool has_horizontal = abs_x > kScrollEpsilon;
    const bool has_vertical = abs_y > kScrollEpsilon;
    if (!has_horizontal && !has_vertical) {
        return;
    }

    const float rounded_y = std::round(abs_y);
    const bool vertical_step =
        has_vertical && rounded_y >= 1.f && std::abs(abs_y - rounded_y) <= kStepEpsilon;
    const bool wheel_like = !has_horizontal && vertical_step;
    const bool strong_smooth_like = has_horizontal || (has_vertical && abs_y < 1.f - kStepEpsilon);
    const bool smooth_like = strong_smooth_like || !vertical_step;

    if (smooth_like) {
        ++smooth_scroll_score;
        wheel_scroll_score = std::max(0, wheel_scroll_score - 1);
    } else if (wheel_like) {
        ++wheel_scroll_score;
        smooth_scroll_score = std::max(0, smooth_scroll_score - 1);
    }

    if (strong_smooth_like || smooth_scroll_score >= kTrackpadScoreThreshold) {
        scroll_input_mode = ScrollInputMode::Trackpad;
    } else if (wheel_scroll_score >= kWheelScoreThreshold) {
        scroll_input_mode = ScrollInputMode::Wheel;
    }
}

void App::Impl::handleScrollEvent(const sapp_event *ev, bool imgui_handled) {
    // A scroll "sequence" is a run of events less than kScrollSequenceGap apart:
    // one continuous physical gesture plus the momentum (kinetic) tail macOS
    // keeps emitting after the fingers lift. Capture the gap before
    // classifyScroll() overwrites last_scroll_time.
    constexpr double kScrollSequenceGap = 0.1;
    const bool new_sequence = last_scroll_time == 0 ||
                              stm_sec(stm_diff(stm_now(), last_scroll_time)) > kScrollSequenceGap;

    classifyScroll(ev->scroll_x, ev->scroll_y);

    const ImGuiIO &io = ImGui::GetIO();
    if (imgui_handled || io.WantCaptureMouse) {
        return;
    }

    // A continuous scroll = one gesture = one mode. Lock the mode (pan / zoom /
    // orbit, set by the held modifiers) at the sequence start. macOS keeps
    // emitting decaying momentum events after the fingers lift, so if the
    // modifiers change during that tail the mode would flip mid-glide and lurch
    // the camera — e.g. ending a Shift pan drops into orbit, or pressing Shift
    // into an orbit's tail jumps into a pan. Once the modifiers diverge from the
    // sequence start, swallow the rest of the tail in either direction.
    const bool pan_now = isPanModifier(ev->modifiers);
    const bool zoom_now = isZoomModifier(ev->modifiers);
    if (new_sequence) {
        scroll_seq_pan = pan_now;
        scroll_seq_zoom = zoom_now;
        scroll_seq_canceled = false;
    }
    if (pan_now != scroll_seq_pan || zoom_now != scroll_seq_zoom) {
        scroll_seq_canceled = true;
    }
    if (scroll_seq_canceled) {
        return;
    }

    pending_scroll_x += ev->scroll_x;
    pending_scroll_y += ev->scroll_y;
    pending_scroll_modifiers = ev->modifiers;
}

bool App::Impl::shouldThrottleIdle() const {
    if (!cfg.pause_when_unfocused) {
        return false;
    }
    // Foreground stays at vsync. Throttling here would add a frame of
    // latency on first interaction for no real power saving — vsync
    // already idles the GPU when nothing is changing.
    if (window_focused && window_visible) {
        return false;
    }
    // Backgrounded but the user is still interacting. Scroll events,
    // mouse moves, and keyboard input fire even when another window
    // owns focus, and onEvent bumps last_activity_time for them — so
    // a recent bump means the user is driving the camera (or typing
    // into ImGui) and we should keep up at full rate.
    constexpr double kIdleAfterInputSeconds = 1.0;
    if (last_activity_time != 0 &&
        stm_sec(stm_diff(stm_now(), last_activity_time)) < kIdleAfterInputSeconds) {
        return false;
    }
    // OS-level drag hover. Set asynchronously by AppKit / browser DOM
    // callbacks (not gated on the frame loop running), so reading the
    // platform's internal state here picks up "files are dragging
    // over" before we'd otherwise tick a throttled frame to notice.
    if (platform_ && platform_->windowState().drag_hover.active) {
        return false;
    }
    // TODO: in-app animation hook. When something like cfg.auto_orbit
    // is producing motion of its own we want to keep the loop at full
    // rate even with no user input. Today auto_orbit only ticks
    // inside onFrame so a throttled frame would still advance the
    // camera at 2 Hz; once the animation system grows beyond a single
    // toggle, wire its "is running" signal in here.
    return true;
}

void App::Impl::updateCameraInput() {
    if (!scene) {
        pending_scroll_x = 0.f;
        pending_scroll_y = 0.f;
        platform_gesture_events.clear();
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        const ImVec2 d = io.MouseDelta;
        // Left-drag: orbit. ~0.005 rad/px feels right at typical 1k-1.5k px windows.
        if (io.MouseDown[0]) {
            camera.orbit(d.x * 0.005f, d.y * 0.005f);
        }
        // Middle-drag: pan. Pixel→world scale tracks zoom distance so motion
        // feels constant at any range.
        if (io.MouseDown[2]) {
            const float scale = camera.distance * 0.001f;
            camera.pan(-d.x * scale, d.y * scale);
        }
        if (pending_scroll_y != 0.f || pending_scroll_x != 0.f) {
            // 1.1^wheel: each notch = 10% closer/further. Matches Blender feel.
            const bool zoom_scroll = scroll_input_mode == ScrollInputMode::Wheel ||
                                     isZoomModifier(pending_scroll_modifiers);
            if (zoom_scroll) {
                const float wheel = pending_scroll_y != 0.f ? pending_scroll_y : pending_scroll_x;
                camera.dolly(std::pow(1.1f, -wheel), scene_radius);
            } else if (isPanModifier(pending_scroll_modifiers)) {
                // Shift + trackpad scroll: pan, mirroring Blender. World-space
                // scale tracks zoom distance so motion feels constant at any
                // range, matching the middle-drag pan above. Signs mirror that
                // path too (negate x so the scene tracks the gesture).
                const float scale = camera.distance * (platform::kIsWeb ? 0.1f : 0.02f);
                camera.pan(-pending_scroll_x * scale, pending_scroll_y * scale);
            } else {
                constexpr float kTrackpadOrbitSensitivity = platform::kIsWeb ? 0.2f : 0.03f;
                camera.orbit(pending_scroll_x * kTrackpadOrbitSensitivity,
                             pending_scroll_y * kTrackpadOrbitSensitivity);
            }
        }
        for (const auto &event : platform_gesture_events) {
            if (event.type == platform::GestureType::PinchUpdate && event.scale_delta > 0.f) {
                camera.dolly(1.f / std::clamp(event.scale_delta, 0.05f, 20.f), scene_radius);
            }
        }
    }
    pending_scroll_x = 0.f;
    pending_scroll_y = 0.f;
    platform_gesture_events.clear();
}

void App::Impl::applyInitialCamera() {
    if (!cfg.initial_camera.has_value()) {
        return;
    }
    camera = *cfg.initial_camera;
    camera.sanitize();
    // Re-derive near/far/distance clamps against the *current* scene radius,
    // not whatever was true when the camera state was saved.
    camera.dolly(1.f, scene_radius);
}

void App::Impl::ensureSceneTarget(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    // Pick the offscreen color format. LDR mode matches the swapchain's
    // format; HDR mode promotes to RGBA16F when the backend can render+blend
    // it. WebGPU is strict about pipeline-vs-attachment format match, so
    // `setTargetColorFormat` rebuilds the scene pipelines on every change.
    sg_environment env = sglue_environment();
    sg_pixel_format swap_fmt = env.defaults.color_format;
    if (swap_fmt == SG_PIXELFORMAT_NONE) {
        swap_fmt = SG_PIXELFORMAT_RGBA8;
    }
    sg_pixel_format depth_fmt = env.defaults.depth_format;
    if (depth_fmt == SG_PIXELFORMAT_NONE) {
        depth_fmt = SG_PIXELFORMAT_DEPTH;
    }
    const sg_pixel_format hdr_fmt = pickHdrColorFormat();
    const sg_pixel_format color_fmt =
        (quality.enable_hdr && hdr_fmt != SG_PIXELFORMAT_NONE) ? hdr_fmt : swap_fmt;
    if (!scene_rt.matches(width, height, color_fmt, depth_fmt)) {
        scene_rt.create(width, height, color_fmt, depth_fmt);
    }
    scene_renderer.setTargetColorFormat(color_fmt);
    cut_renderer.setTargetColorFormat(color_fmt);

    // AO targets, allocated only when AO is enabled. Sized to ao_resolution_scale
    // of the *scaled* scene resolution (the width/height passed in) — coupled to
    // the depth buffer the GTAO pass reads. Computing AO at a different
    // resolution than its source depth quantizes the normal reconstruction +
    // horizon march to the depth-texel grid and looks blocky, so the AO must
    // track the scene resolution, not the window.
    //
    // The flicker that coupling-to-scene used to cause (each scale step
    // reallocated the shared history target and reset the frame-late history,
    // dropping AO for a frame) is solved by double-buffering: we resize the
    // *write* history buffer (1 - ao_hist_last_written) and the raw target here,
    // but never the *read* buffer the scene pass is about to sample. The read
    // buffer keeps its previous-resolution contents, which sample fine via the
    // scene shader's normalized-UV lookup. So a scale step never drops AO.
    float ao_scale = quality.ao_resolution_scale;
    ao_scale = ao_scale < 0.25f ? 0.25f : (ao_scale > 1.0f ? 1.0f : ao_scale);
    const auto ao_dim = [ao_scale](uint32_t d) -> uint32_t {
        const auto s = static_cast<uint32_t>(static_cast<float>(d) * ao_scale + 0.5f);
        return s < 1u ? 1u : s;
    };
    const uint32_t ao_w = ao_dim(width);
    const uint32_t ao_h = ao_dim(height);

    const sg_pixel_format ao_fmt = pickAoColorFormat();
    if (quality.enable_ao) {
        const int write_idx = 1 - ao_hist_last_written;
        // raw is only needed when denoise is on (GTAO → raw → denoise → history).
        if (quality.enable_ao_denoise) {
            if (!ao_rt_raw.matches(ao_w, ao_h, ao_fmt)) {
                ao_rt_raw.create(ao_w, ao_h, ao_fmt);
            }
        } else if (ao_rt_raw.color.id != SG_INVALID_ID) {
            ao_rt_raw.destroy();
        }
        // Resize only the write buffer; leave the read buffer (which this frame's
        // scene pass samples) at whatever size it last held.
        if (!ao_hist[write_idx].matches(ao_w, ao_h, ao_fmt)) {
            ao_hist[write_idx].create(ao_w, ao_h, ao_fmt);
        }
        // No valid history to sample until the read buffer has been written.
        if (ao_hist[ao_hist_last_written].color.id == SG_INVALID_ID) {
            ao_history_valid = false;
        }
        ao_pass.setTargetColorFormat(ao_fmt);
        ao_denoise_pass.setTargetColorFormat(ao_fmt);
    } else {
        // AO off — free all AO targets (destroy() is a no-op once freed) and
        // invalidate. Re-enabling reallocates on the next frame.
        ao_rt_raw.destroy();
        ao_hist[0].destroy();
        ao_hist[1].destroy();
        ao_history_valid = false;
    }
}

void App::Impl::render() {
    const uint64_t render_submit_start = stm_now();
    scene_submit_ms = 0.0;
    bool scene_consumed_ao_this_frame = false;

    // While a screenshot export is rendering, run the whole pipeline at the
    // export resolution with the maxed quality snapshot. We temporarily swap the
    // live `quality` for the duration of render() (the UI was already built this
    // frame, so it still shows the user's live settings) and restore it before
    // returning. The dims override happens below, and the captured frame gets an
    // extra composite into `export_out_rt` after the swapchain pass.
    const bool exporting_frame = export_render_active;
    RenderQualitySettings saved_quality;
    if (exporting_frame) {
        saved_quality = quality;
        quality = export_quality;
    }

    // Drive the chunked GPU uploads BEFORE sg_begin_pass so any new sokol
    // buffer creation isn't tangled up with the active scene pass. The base and
    // cut scenes live in separate renderers, so their uploads run independently.
    if (scene && !scene_uploaded) {
        if (!scene_renderer.uploadInProgress()) {
            scene_renderer.beginUpload(scene);
        }
        if (scene_renderer.advanceUpload()) {
            scene_uploaded = true;
        }
    }
    if (cut_scene && !cut_uploaded) {
        if (!cut_renderer.uploadInProgress()) {
            cut_renderer.beginUpload(cut_scene);
        }
        if (cut_renderer.advanceUpload()) {
            cut_uploaded = true;
        }
    }

    // Lazily allocate (or reallocate on resize / DPI change) the offscreen
    // scene target. Doing this here, just before the pass begins, avoids
    // reallocating from inside an event callback while a frame may still be
    // in flight.
    // Render the offscreen scene/AO/denoise passes at quality.render_scale of the
    // window resolution; the composite pass upsamples to the swapchain. This is a
    // fragment-count lever across every offscreen pass — the dominant GPU cost.
    // All passes already size off scene_rt's dimensions, so scaling here is
    // self-consistent (incl. the scene shader's gl_FragCoord-based AO sampling,
    // which uses scene_rt.width/height). Clamp to a sane range.
    // Detect interaction this frame. Two sources, both of which re-render the
    // scene and should drop resolution:
    //   - a camera change (orbit/pan/zoom/fov/projection), and
    //   - any active ImGui widget. IsAnyItemActive() is true for the whole time
    //     a slider is held, so dragging the wedge/angle cut, exposure, AO, etc.
    //     keeps us in the low-res interactive state until release. (The cut
    //     sliders feed the shader live, so they're exactly as interactive as
    //     the camera.)
    // The camera snapshot is refreshed every frame — even when dynamic scaling
    // is off — so toggling it back on doesn't see a false change.
    const bool cam_changed = camera.yaw != dyn_prev_yaw || camera.pitch != dyn_prev_pitch ||
                             camera.distance != dyn_prev_distance ||
                             camera.fov_deg != dyn_prev_fov || camera.target != dyn_prev_target ||
                             camera.projection != dyn_prev_proj;
    dyn_prev_yaw = camera.yaw;
    dyn_prev_pitch = camera.pitch;
    dyn_prev_distance = camera.distance;
    dyn_prev_fov = camera.fov_deg;
    dyn_prev_target = camera.target;
    dyn_prev_proj = camera.projection;
    const bool interacting = cam_changed || ImGui::IsAnyItemActive();

    const auto clamp_scale = [](float s) { return s < 0.25f ? 0.25f : (s > 4.0f ? 4.0f : s); };
    float render_scale;
    if (quality.dynamic_render_scale) {
        float lo = clamp_scale(quality.render_scale_min);
        float hi = clamp_scale(quality.render_scale_max);
        if (lo > hi) {
            const float t = lo;
            lo = hi;
            hi = t;
        }
        // While the camera moves the scale stays low; once it settles it jumps
        // straight to `hi`. The settle transition is a single step (not a ramp
        // through intermediate resolutions) because each step reallocates the
        // offscreen targets *and* resets the AO temporal history — a staircase
        // of AO re-converges on a still image reads as flicker. One jump = one
        // realloc + one AO reset per settle, and it avoids reallocating through
        // huge intermediate targets now that `hi` can reach 4x.
        constexpr double kSettleDelaySeconds = 0.2;
        if (interacting) {
            if (quality.adaptive_render_scale) {
                // Closed-loop: hold the highest scale in [lo, hi] that meets the
                // target frame time. frame_interval_ms is last frame's wall time
                // (its render cost); smooth it so a single hitch doesn't yank the
                // scale. React asymmetrically — drop fast when over budget, probe
                // up slowly — and rate-limit changes so we don't reallocate every
                // frame near the budget boundary (during motion the resolution
                // step and AO reset are both masked, but churn still costs time).
                const double target_ms = 1000.0 / std::max(15.0f, quality.render_scale_target_fps);
                if (!dyn_was_moving) {
                    // Interaction just started: resume from the last sustainable
                    // in-motion scale rather than dropping to the floor and
                    // re-climbing the staircase — repeated orbits shouldn't keep
                    // re-running the ramp. Re-seed the average at target and clear
                    // the climb/hold locks so the controller can react right away,
                    // and so the slow full-res settled frame we just showed
                    // doesn't drag the controller down. (dyn_motion_scale is left
                    // as-is and clamped to [lo, hi] below; on the very first
                    // interaction it starts from its default.)
                    dyn_frame_ms_ema = target_ms;
                    dyn_climb_lock = 0;
                    dyn_last_scale_change = 0;
                } else {
                    constexpr double kAlpha = 0.25;           // EMA smoothing
                    constexpr double kClimbLockSeconds = 1.5; // pause up-probing after overshoot
                    // Minimum wall time between applied scale changes, in either
                    // direction. The controller still samples every frame, but it
                    // only reallocates the offscreen targets this often — so a
                    // steady orbit holds a resolution instead of churning through
                    // fine steps. A severe overshoot bypasses this to recover from
                    // a real hitch immediately.
                    constexpr double kScaleHoldSeconds = 0.4;
                    dyn_frame_ms_ema =
                        dyn_frame_ms_ema * (1.0 - kAlpha) + frame_interval_ms * kAlpha;
                    const bool hold_elapsed =
                        stm_sec(stm_diff(stm_now(), dyn_last_scale_change)) > kScaleHoldSeconds;
                    if (dyn_frame_ms_ema > target_ms * 1.6) {
                        // Severely over budget — drop hard immediately, bypassing
                        // the hold so a real hitch recovers at once, and stop
                        // probing upward: we just found the ceiling.
                        dyn_motion_scale -= 0.5f;
                        dyn_climb_lock = stm_now();
                        dyn_last_scale_change = stm_now();
                    } else if (dyn_frame_ms_ema > target_ms * 1.15) {
                        // Mildly over budget — coarsen, but no more often than the
                        // hold so we don't reallocate every frame near the
                        // boundary. Lock upward probing regardless.
                        if (hold_elapsed) {
                            dyn_motion_scale -= 0.125f;
                            dyn_last_scale_change = stm_now();
                        }
                        dyn_climb_lock = stm_now();
                    } else if (hold_elapsed &&
                               stm_sec(stm_diff(stm_now(), dyn_climb_lock)) > kClimbLockSeconds) {
                        // Under budget, or vsync-capped at it — probe finer. If
                        // this step overshoots, the branch above pulls it back and
                        // locks probing, so it settles at the sustainable scale.
                        dyn_motion_scale += 0.125f;
                        dyn_last_scale_change = stm_now();
                    }
                }
                dyn_motion_scale =
                    dyn_motion_scale < lo ? lo : (dyn_motion_scale > hi ? hi : dyn_motion_scale);
                dyn_scale = dyn_motion_scale;
            } else {
                // Non-adaptive: fixed floor while moving (blurry-but-smooth).
                dyn_scale = lo;
            }
            dyn_settle_anchor = stm_now();
        } else if (stm_sec(stm_diff(stm_now(), dyn_settle_anchor)) >= kSettleDelaySeconds) {
            // Settled and not interacting (auto-orbit counts as interaction, so
            // it never lands here): jump to `hi` unconditionally — the still
            // image is allowed to exceed the frame-time budget to maximize
            // fidelity, since slowness no longer costs interactivity.
            dyn_scale = hi;
        }
        dyn_was_moving = interacting;
        render_scale = dyn_scale;
    } else {
        render_scale = clamp_scale(quality.render_scale);
        dyn_scale = render_scale;
    }
    // Don't let a high settled scale (up to 4x) push the offscreen target past
    // the backend's max texture size on a large window. Cap uniformly so the
    // aspect ratio is preserved.
    const float max_tex = static_cast<float>(sg_query_limits().max_image_size_2d);
    if (max_tex > 0.f && fb_width > 0 && fb_height > 0) {
        const float cap = std::min(max_tex / static_cast<float>(fb_width),
                                   max_tex / static_cast<float>(fb_height));
        if (render_scale > cap) {
            render_scale = cap;
        }
    }
    // Cap the scale so the resolution-scaling offscreen targets stay within a
    // memory budget — protects memory-constrained backends from OOMing on a
    // large window times a high settled scale. The targets that grow with
    // resolution are scene color + scene depth (full res) and, when AO is on,
    // the two AO targets at ao_resolution_scale² of the scene area. Everything
    // else (IBL cubemaps, the 1x1 AO dummy, the swapchain) is fixed-size and
    // excluded. The per-pixel cost mirrors the format choices ensureSceneTarget
    // makes below, so the estimate tracks HDR / AO toggles automatically.
    if (quality.render_scale_memory_budget_mb > 0.f && fb_width > 0 && fb_height > 0) {
        const auto bytes_per_pixel = [](sg_pixel_format f) -> float {
            switch (f) {
            case SG_PIXELFORMAT_RGBA16F:
                return 8.f;
            case SG_PIXELFORMAT_RGBA8:
                return 4.f;
            default:
                return 4.f; // depth32f / depth-stencil
            }
        };
        const sg_environment env = sglue_environment();
        const sg_pixel_format swap_fmt = env.defaults.color_format == SG_PIXELFORMAT_NONE
                                             ? SG_PIXELFORMAT_RGBA8
                                             : env.defaults.color_format;
        const sg_pixel_format depth_fmt = env.defaults.depth_format == SG_PIXELFORMAT_NONE
                                              ? SG_PIXELFORMAT_DEPTH
                                              : env.defaults.depth_format;
        const sg_pixel_format hdr_fmt = pickHdrColorFormat();
        const sg_pixel_format color_fmt =
            (quality.enable_hdr && hdr_fmt != SG_PIXELFORMAT_NONE) ? hdr_fmt : swap_fmt;
        float bytes_per_scene_px = bytes_per_pixel(color_fmt) + bytes_per_pixel(depth_fmt);
        if (quality.enable_ao) {
            const float ao_scale = std::clamp(quality.ao_resolution_scale, 0.25f, 1.0f);
            // AO is coupled to the scaled scene resolution: two ping-pong history
            // buffers, plus the raw target when denoise is on (three total),
            // each at ao_scale² of the scene area.
            const float ao_buffers = quality.enable_ao_denoise ? 3.f : 2.f;
            bytes_per_scene_px +=
                ao_buffers * bytes_per_pixel(pickAoColorFormat()) * ao_scale * ao_scale;
        }
        const double budget_bytes =
            static_cast<double>(quality.render_scale_memory_budget_mb) * 1024.0 * 1024.0;
        const double base_px = static_cast<double>(fb_width) * static_cast<double>(fb_height);
        // total_bytes(scale) = base_px * scale² * bytes_per_scene_px ≤ budget.
        const double max_scale_sq = budget_bytes / (base_px * bytes_per_scene_px);
        float budget_cap = static_cast<float>(std::sqrt(std::max(0.0, max_scale_sq)));
        if (budget_cap < 0.25f) {
            budget_cap = 0.25f; // never strangle below the hard floor, even on a tiny budget
        }
        if (render_scale > budget_cap) {
            render_scale = budget_cap;
        }
    }
    const auto scale_dim = [render_scale](uint32_t d) -> uint32_t {
        const auto s = static_cast<uint32_t>(static_cast<float>(d) * render_scale + 0.5f);
        return s < 1u ? 1u : s;
    };
    // During an export the offscreen targets are driven straight from the export
    // resolution (the dynamic-scale math above is moot — export_quality disables
    // it — and its result is overridden here). The scene/composite aspect ratio
    // follows scene_rt's dimensions, so this also gives the exported frame the
    // requested output aspect, independent of the window.
    const uint32_t want_w = exporting_frame ? export_internal_w : scale_dim(fb_width);
    const uint32_t want_h = exporting_frame ? export_internal_h : scale_dim(fb_height);
    // A resize / scale change reallocates scene_rt; if it does we MUST render
    // into it this frame (an empty target can't be composited).
    const bool scene_target_resized = scene_rt.width != want_w || scene_rt.height != want_h;
    ensureSceneTarget(want_w, want_h);

    // Decide whether to re-run the expensive scene + AO passes this frame. With
    // pause_when_static on, an unchanged view reuses the cached scene_rt and
    // only the cheap composite + ImGui run below — the UI stays fully live while
    // the geometry renders on demand. We keep rendering through a settle window
    // after the last change so the AO temporal denoise and the dynamic-scale
    // settle jump converge before we hold the frame.
    bool render_scene = true;
    if (quality.pause_when_static) {
        const auto session_phase = build_session.phase();
        const bool jobs_running = !ibl_installed || build_in_progress ||
                                  (scene && !scene_uploaded) || (cut_scene && !cut_uploaded) ||
                                  pending_cut_rebuild || session_phase == BuildPhase::Walking ||
                                  session_phase == BuildPhase::ResolvedReady;
        const bool ibl_dirty = ibl_rebake_pending || ibl_settings != ibl_last_baked_settings;
        const bool scene_changing = interacting || jobs_running || ibl_dirty;
        if (scene_changing) {
            last_scene_change = stm_now();
        }
        constexpr double kSceneStableSeconds = 1.0;
        const bool converging =
            last_scene_change != 0 &&
            stm_sec(stm_diff(stm_now(), last_scene_change)) < kSceneStableSeconds;
        render_scene = scene_changing || converging || scene_target_resized;
    }

    if (render_scene) {
        // Pass 1 — scene into offscreen color + depth. Depth-clear convention
        // is backend-conditional (see useReversedZ): 0.0 paired with
        // GREATER_EQUAL on `[0,1]` clip-depth backends, 1.0 paired with
        // LESS_EQUAL on GLES3.
        sg_pass scene_pass{};
        scene_pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        scene_pass.action.colors[0].clear_value = {0.125f, 0.157f, 0.188f, 1.0f}; // 0x202830
        scene_pass.action.depth.load_action = SG_LOADACTION_CLEAR;
        scene_pass.action.depth.clear_value = useReversedZ() ? 0.0f : 1.0f;
        // STORE so the composite pass's depth-debug view can sample the
        // depth attachment after the scene pass ends.
        scene_pass.action.depth.store_action = SG_STOREACTION_STORE;
        scene_pass.attachments = scene_rt.passAttachments();
        scene_pass.label = "scene_pass";
        // This is the frame's first sg_begin_pass, where sokol's Metal backend waits
        // on the in-flight-frames semaphore (SG_NUM_INFLIGHT_FRAMES=2) until the GPU
        // drains an older frame. When the GPU is the bottleneck the CPU blocks here,
        // so this is GPU backpressure — not command encoding — even though it lands
        // inside encode_ms. Time it separately so the panel attributes it honestly.
        const uint64_t gpu_wait_start = stm_now();
        sg_begin_pass(&scene_pass);
        gpu_wait_ms = stm_sec(stm_diff(stm_now(), gpu_wait_start)) * 1000.0;

        if (scene && scene_uploaded) {
            if (!camera_framed) {
                glm::vec3 bmin{0.f}, bmax{0.f};
                if (scene_renderer.worldBounds(bmin, bmax)) {
                    scene_radius = camera.frameBounds(bmin, bmax);
                    applyInitialCamera();
                    camera_framed = true;
                }
            }
            // Choose between the base scene (interactive shader-cut preview) and the
            // baked Boolean-cut scene. The cut scene is shown only when the Boolean
            // cut is enabled and a resident bake matches the committed angle —
            // otherwise (cut disabled, mid-drag, or a rebuild in flight) we show the
            // base scene with the live shader cut. The two cut methods are never
            // active together, which is what avoids the discard-vs-cut-face z-fight.
            const bool cut_ready = cfg.boolean_cut && cut_uploaded &&
                                   cut_built_start_deg == cfg.angle_cut_start_deg &&
                                   cut_built_end_deg == cfg.angle_cut_end_deg;
            SceneRenderer &renderer = cut_ready ? cut_renderer : scene_renderer;
            active_renderer = &renderer;
            // Flipping which scene is drawn changes the geometry under the temporal
            // AO history, so invalidate it for one frame to avoid ghosting.
            if (cut_ready != last_shown_cut) {
                ao_history_valid = false;
                last_shown_cut = cut_ready;
            }

            SceneRenderer::RenderFlags flags;
            flags.cull = cfg.cull;
            if (cut_ready) {
                // Geometry already carries watertight cut faces — no shader discard.
                flags.angle_cut = false;
                flags.shader_angle_cut = false;
            } else if (cfg.boolean_cut) {
                // Boolean cut enabled but its bake isn't current (dragging / baking):
                // preview the cut interactively with the shader discard.
                flags.angle_cut = true;
                flags.shader_angle_cut = true;
            } else {
                // No Boolean cut — honour the standalone shader/instance cut toggles.
                flags.angle_cut = cfg.angle_cut;
                flags.shader_angle_cut = cfg.shader_angle_cut;
            }
            flags.angle_cut_start_deg = cfg.angle_cut_start_deg;
            flags.angle_cut_end_deg = cfg.angle_cut_end_deg;
            flags.enable_pbr = cfg.enable_pbr;
            // Single source of truth for sun direction: ibl_settings.sun_dir
            // drives both the IBL bake and the analytical light, so the baked
            // reflected sun lines up with the analytical highlight (docs §9.1).
            flags.sun_dir = ibl_settings.sun_dir;
            flags.sun_intensity = ibl_settings.sun_intensity;
            // Feed previous frame's denoised AO+bent-normal map into the scene
            // shader's PBR IBL path. Gated on:
            //   * PBR (Lambert ignores AO inside the scene shader)
            //   * `enable_advanced_ao` — the user-facing A/B toggle. When off,
            //     the scene shader falls back to no-AO defaults and the
            //     composite does the legacy single-multiply on the AO scalar.
            //   * History validity (first frame after enable / resize)
            //   * Non-debug view
            // When any gate fails the binding still points at a valid texture
            // (BRDF LUT as placeholder, see scene_renderer.cpp) but the FS
            // uniform disables the sample — keeps the binding contract trivial.
            const bool ao_history_pbr = cfg.enable_pbr && quality.enable_ao &&
                                        quality.enable_advanced_ao &&
                                        quality.debug_view == DebugView::Off && ao_history_valid;
            // Frame-late read: sample the buffer the previous frame wrote (the
            // GTAO/denoise passes below write the *other* buffer, so this read
            // isn't clobbered, and a scale step that resized only the write
            // buffer leaves this one intact). Falls back to a null view when
            // history isn't usable; scene shader's enable bit gates the sample.
            const AoRenderTarget &history_src = ao_hist[ao_hist_last_written];
            const bool history_src_valid = history_src.color.id != SG_INVALID_ID;
            const bool ao_history_active = ao_history_pbr && history_src_valid;
            flags.ao_history_view = ao_history_active ? history_src.color_texture_view : sg_view{};
            flags.ao_history_sampler = ao_history_active ? history_src.sampler : sg_sampler{};
            flags.ao_history_enable = ao_history_active;
            flags.ao_bent_strength = quality.ao_bent_strength;
            // Remember the actual decision the scene path made so the composite
            // gate matches. Naively re-deriving from `ao_history_valid` later
            // would mis-fire on the first frame after enable (history flips
            // valid *between* the scene render and the composite, so a re-
            // computed gate would tell the composite "scene applied AO" when
            // it actually did not).
            scene_consumed_ao_this_frame = ao_history_pbr;
            const uint64_t scene_submit_start = stm_now();
            renderer.render(camera, scene_rt.width, scene_rt.height, flags);
            scene_submit_ms = stm_sec(stm_diff(stm_now(), scene_submit_start)) * 1000.0;
        }

        sg_end_pass();

        // Pass 2 (optional) — GTAO, then an optional bilateral denoise, with the
        // final AO landing in the ping-pong *write* buffer (the one the scene
        // pass above did NOT just sample). Skipped entirely when AO is disabled
        // or a depth-debug view is active.
        //
        // With denoise on: GTAO writes ao_rt_raw, then the denoise reads raw +
        // scene depth and writes ao_hist[write]. With denoise off: GTAO writes
        // ao_hist[write] directly (no raw needed). Either way the write buffer
        // ends up holding this frame's final AO; we then mark it as the latest,
        // so next frame's scene pass reads it (frame-late) and this frame's
        // composite reads it (in-frame, for the Lambert path).
        //
        // AO is recomputed every rendered frame so it tracks the geometry as the
        // camera moves (only one frame late, which is imperceptible). We do NOT
        // freeze it during motion — a frozen screen-space AO smears across the
        // moving geometry (ghosting). Dynamic-scale flicker is handled by the
        // ping-pong (see ensureSceneTarget): the scale step resized the write
        // buffer, never the read buffer the scene just sampled.
        const int ao_write_idx = 1 - ao_hist_last_written;
        AoRenderTarget &ao_write = ao_hist[ao_write_idx];
        const bool ao_active = quality.enable_ao && quality.debug_view == DebugView::Off &&
                               ao_write.color.id != SG_INVALID_ID && scene_uploaded &&
                               (!quality.enable_ao_denoise || ao_rt_raw.color.id != SG_INVALID_ID);
        if (ao_active) {
            // GTAO target: the raw scratch when we'll denoise, else the history
            // buffer directly.
            AoRenderTarget &gtao_target = quality.enable_ao_denoise ? ao_rt_raw : ao_write;
            sg_pass ao_pass_desc{};
            ao_pass_desc.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            ao_pass_desc.attachments = gtao_target.passAttachments();
            ao_pass_desc.label = "ao_pass";
            sg_begin_pass(&ao_pass_desc);
            ao_pass.draw(scene_rt, camera, gtao_target.width, gtao_target.height, quality);
            sg_end_pass();

            if (quality.enable_ao_denoise) {
                sg_pass denoise_pass_desc{};
                denoise_pass_desc.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
                denoise_pass_desc.attachments = ao_write.passAttachments();
                denoise_pass_desc.label = "ao_denoise_pass";
                sg_begin_pass(&denoise_pass_desc);
                ao_denoise_pass.draw(ao_rt_raw, scene_rt, camera, ao_write.width, ao_write.height);
                sg_end_pass();
            }

            // The write buffer now holds this frame's final AO — promote it to
            // "latest" so the scene (next frame) and composite (this frame) read
            // it, and the next frame writes the other buffer.
            ao_hist_last_written = ao_write_idx;
            ao_history_valid = true;
        }

        // Cache the scene pass's AO-consumed decision so a later cached-scene
        // composite (when we skip the scene pass) gates the AO multiply the same
        // way the cached scene_rt was actually rendered.
        last_scene_consumed_ao = scene_consumed_ao_this_frame;
    } else {
        // Reusing the cached scene_rt + AO targets — no offscreen GPU work this
        // frame; only the composite + ImGui below run.
        gpu_wait_ms = 0.0;
        scene_submit_ms = 0.0;
    }

    // Split the submit timer here: everything above is pure CPU encoding of the
    // offscreen passes; everything below starts with the swapchain drawable
    // acquisition (sglue_swapchain), which on Metal blocks on GPU backpressure
    // when the GPU is behind. Splitting at this boundary separates real CPU
    // submission cost (encode_ms) from the GPU-bound stall (present_ms).
    const uint64_t present_start = stm_now();

    // Pass 3 — composite the offscreen target into the swapchain, then
    // ImGui on top. The composite covers the entire viewport so we
    // don't need to clear color or depth first.
    //
    // The composite inputs (inv_view_proj + the AO target) are computed once,
    // outside the pass, so the export path can reuse them for a second composite
    // into export_out_rt after the swapchain pass.
    //
    // Match the conventions the scene shader uses (see scene_renderer.cpp):
    // GL/GLES needs [-1,1] z; everything else [0,1]. Reversed-Z is gated by
    // useReversedZ(). inv(view_proj) lets the composite FS turn a screen-space
    // pixel into a world-space view ray for the background dome.
    const sg_backend backend = sg_query_backend();
    const bool homogeneous_depth = (backend == SG_BACKEND_GLCORE) || (backend == SG_BACKEND_GLES3);
    const float aspect = (scene_rt.height > 0) ? static_cast<float>(scene_rt.width) /
                                                     static_cast<float>(scene_rt.height)
                                               : 1.0f;
    const glm::mat4 view_proj =
        camera.proj(aspect, homogeneous_depth, useReversedZ()) * camera.view();
    const glm::mat4 inv_view_proj = glm::inverse(view_proj);
    // Composite samples the latest final-AO buffer for its Lambert AO multiply —
    // the same data the next frame's scene-shader PBR path will sample, kept
    // consistent so toggling PBR on/off doesn't introduce a frame of mismatched
    // AO. `last_scene_consumed_ao` mirrors the scene decision (captured before
    // the AO+denoise passes ran) so the first frame after enable still gets a
    // composite AO multiply. When no history has been written, an empty target
    // makes composite's `ao_on` check fall back to its 1×1 white dummy. Sourced
    // from persistent members so this works for a cached scene too.
    const AoRenderTarget *composite_ao_src =
        ao_history_valid ? &ao_hist[ao_hist_last_written] : nullptr;
    const AoRenderTarget composite_ao_empty{};
    const AoRenderTarget &composite_ao_target =
        composite_ao_src ? *composite_ao_src : composite_ao_empty;

    sg_pass swap_pass{};
    swap_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    swap_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
    swap_pass.swapchain = sglue_swapchain();
    swap_pass.label = "swapchain_pass";
    sg_begin_pass(&swap_pass);
    composite.draw(scene_rt, composite_ao_target, ao_pass, quality, last_scene_consumed_ao,
                   camera.near_plane, camera.far_plane, scene_renderer.iblPrefilterView(),
                   scene_renderer.iblCubeSampler(), inv_view_proj, camera.eye());
    ImGui_ImplSokol_Render();
    sg_end_pass();

    // Export capture: composite the same converged frame into the full-res
    // offscreen LDR target (no ImGui). exportPostRender reads it back next.
    if (exporting_frame && export_capture && export_out_rt.color.id != SG_INVALID_ID) {
        sg_pass export_pass{};
        export_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
        export_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
        export_pass.attachments = export_out_rt.passAttachments();
        export_pass.label = "export_pass";
        sg_begin_pass(&export_pass);
        composite.draw(scene_rt, composite_ao_target, ao_pass, quality, last_scene_consumed_ao,
                       camera.near_plane, camera.far_plane, scene_renderer.iblPrefilterView(),
                       scene_renderer.iblCubeSampler(), inv_view_proj, camera.eye());
        sg_end_pass();
    }

    sg_commit();
    if (exporting_frame) {
        quality = saved_quality;
    }
    // sg_commit() rotates cur_frame → prev_frame, so the frame we just
    // submitted is in prev_frame (cur_frame is now cleared for the next one).
    render_stats = sg_query_stats().prev_frame;
    const uint64_t render_end = stm_now();
    encode_ms = stm_sec(stm_diff(present_start, render_submit_start)) * 1000.0;
    present_ms = stm_sec(stm_diff(render_end, present_start)) * 1000.0;
    render_submit_ms = stm_sec(stm_diff(render_end, render_submit_start)) * 1000.0;
}

void App::Impl::requestPngExport(const PngExportSettings &settings, std::string explicit_path,
                                 bool quit_when_done) {
    if (export_phase != ExportPhase::Idle) {
        notifications.warning("A screenshot export is already in progress");
        return;
    }
    if (!scene || !scene_uploaded) {
        notifications.error("Load a scene before exporting a screenshot");
        return;
    }

    export_explicit_path = std::move(explicit_path);
    export_quit_when_done = quit_when_done;
    export_settings = settings;
    // Clamp the output and derive the internal (supersampled) resolution. Reduce
    // the supersample factor as needed so the internal target fits the backend's
    // max texture size — keeping the downscale an exact integer ratio.
    uint32_t out_w = std::clamp<uint32_t>(export_settings.out_width, 16u, 16384u);
    uint32_t out_h = std::clamp<uint32_t>(export_settings.out_height, 16u, 16384u);
    uint32_t ss =
        std::clamp<uint32_t>(export_settings.supersample, 1u, PngExportSettings::kMaxSupersample);
    const uint32_t max_tex = static_cast<uint32_t>(sg_query_limits().max_image_size_2d);
    if (max_tex > 0) {
        out_w = std::min(out_w, max_tex);
        out_h = std::min(out_h, max_tex);
        while (ss > 1 && (out_w * ss > max_tex || out_h * ss > max_tex)) {
            --ss;
        }
    }
    export_settings.out_width = out_w;
    export_settings.out_height = out_h;
    export_settings.supersample = ss;
    export_internal_w = out_w * ss;
    export_internal_h = out_h * ss;

    // Maxed quality snapshot for the export frames. Start from the live settings
    // to keep the user's "look" (tonemap curve, exposure, contrast, saturation,
    // sun) and force every cost/quality lever to its best.
    export_quality = quality;
    export_quality.dynamic_render_scale = false;
    export_quality.render_scale = 1.0f;
    export_quality.render_scale_memory_budget_mb = 0.0f; // no memory cap for a one-shot export
    export_quality.cap_fps = false;
    export_quality.pause_when_static = false;
    export_quality.enable_fxaa = true;
    export_quality.fxaa_quality = FxaaQualityPreset::Ultra;
    export_quality.enable_ao = true;
    export_quality.ao_quality = AoQualityPreset::Ultra;
    export_quality.ao_resolution_scale = 1.0f;
    export_quality.enable_ao_denoise = true;
    export_quality.enable_advanced_ao = true;
    export_quality.enable_tonemap = true;
    export_quality.debug_view = DebugView::Off;
    if (hdrSupported()) {
        export_quality.enable_hdr = true;
    }

    export_converge_count = 0;
    export_readback_started = false;
    export_readback.reset();
    export_filename = makeScreenshotFilename();
    export_phase = ExportPhase::Rendering;
    export_progress = notifications.startProgress("Rendering screenshot...");
    std::println("viewer: PNG export started ({}×{} ×{} SSAA → {}×{})", out_w, out_h, ss, out_w,
                 out_h);
}

void App::Impl::exportPreRender() {
    export_render_active = false;
    export_capture = false;
    if (export_phase != ExportPhase::Rendering) {
        return;
    }
    // Allocate the composite output target at the export resolution, using the
    // swapchain's color/depth formats so the composite pipeline (which bakes the
    // swapchain formats) validates against it. Created here, before render()
    // begins any pass.
    sg_environment env = sglue_environment();
    sg_pixel_format color_fmt = env.defaults.color_format;
    if (color_fmt == SG_PIXELFORMAT_NONE) {
        color_fmt = SG_PIXELFORMAT_RGBA8;
    }
    sg_pixel_format depth_fmt = env.defaults.depth_format;
    if (depth_fmt == SG_PIXELFORMAT_NONE) {
        depth_fmt = SG_PIXELFORMAT_DEPTH;
    }
    if (!export_out_rt.matches(export_internal_w, export_internal_h, color_fmt, depth_fmt)) {
        export_out_rt.create(export_internal_w, export_internal_h, color_fmt, depth_fmt);
    }
    export_render_active = true;
    // Render a handful of export-res frames so the GTAO temporal denoise (and the
    // frame-late AO history the PBR path samples) converge before we capture.
    constexpr int kConvergeFrames = 8;
    export_capture = (export_converge_count >= kConvergeFrames);
}

void App::Impl::exportPostRender() {
    switch (export_phase) {
    case ExportPhase::Idle:
        return;
    case ExportPhase::Rendering:
        if (export_capture) {
            // The capture composite was issued this frame; wait for its GPU work
            // to drain before reading back. A few normal frames in between let
            // sokol's in-flight semaphore guarantee the capture frame completed.
            export_phase = ExportPhase::WaitGpu;
            export_wait_count = 3;
        } else {
            ++export_converge_count;
        }
        return;
    case ExportPhase::WaitGpu:
        if (--export_wait_count <= 0) {
            export_phase = ExportPhase::Readback;
            export_readback_started = false;
        }
        return;
    case ExportPhase::Readback: {
        if (!export_readback_started) {
            export_readback_started = true;
            if (!export_readback.begin(export_out_rt.color, export_internal_w, export_internal_h,
                                       export_out_rt.color_format)) {
                finishExport(false, "Screenshot readback is not supported on this build");
                return;
            }
        }
        std::vector<std::uint8_t> pixels;
        const ReadbackStatus st = export_readback.poll(pixels);
        if (st == ReadbackStatus::Pending) {
            return; // try again next frame
        }
        if (st != ReadbackStatus::Ready) {
            finishExport(false, "Screenshot GPU readback failed");
            return;
        }
        // SSAA resolve (box-downscale by the supersample factor) → PNG → deliver.
        const uint32_t ss = export_settings.supersample;
        auto small = downscaleBoxRgba8(pixels, export_internal_w, export_internal_h, ss);
        const uint32_t out_w = export_internal_w / ss;
        const uint32_t out_h = export_internal_h / ss;
        auto png = encodePngRgba8(small, out_w, out_h);
        if (png.empty()) {
            finishExport(false, "Screenshot PNG encoding failed");
            return;
        }
        auto dest = deliverExport(png);
        if (!dest) {
            finishExport(false, "Failed to save screenshot");
            return;
        }
        const bool downloaded = platform::kIsWeb && export_explicit_path.empty();
        finishExport(true, (downloaded ? "Downloaded " : "Saved ") + *dest);
        return;
    }
    }
}

std::optional<std::string> App::Impl::deliverExport(const std::vector<std::uint8_t> &png) {
    // Headless CLI path: write straight to the requested file.
    if (!export_explicit_path.empty()) {
        std::ofstream out{export_explicit_path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::nullopt;
        }
        out.write(reinterpret_cast<const char *>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        if (!out) {
            return std::nullopt;
        }
        return export_explicit_path;
    }
    // Interactive path: native writes to cwd, web triggers a download.
    return platform_->saveExportedImage(
        export_filename, std::as_bytes(std::span<const std::uint8_t>{png.data(), png.size()}));
}

void App::Impl::finishExport(bool ok, const std::string &message) {
    if (export_progress != 0) {
        if (ok) {
            notifications.finishProgress(export_progress, message);
        } else {
            notifications.cancelProgress(export_progress);
        }
        export_progress = 0;
    }
    if (ok) {
        std::println("viewer: PNG export complete — {}", message);
    } else {
        std::println(stderr, "viewer: PNG export failed — {}", message);
        notifications.error(message);
    }
    export_readback.reset();
    export_render_active = false;
    export_capture = false;
    export_phase = ExportPhase::Idle;
    // The export target can be large; free it until the next export.
    export_out_rt.destroy();

    const bool quit_now = export_quit_when_done;
    export_explicit_path.clear();
    export_quit_when_done = false;
    if (quit_now) {
        // Headless screenshot mode: shut the window down once the file is out.
        sapp_quit();
    }
}

std::string App::Impl::browserUrlStateQuery() const {
    std::string query;
    if (cfg.cull != CullOverride::Auto) {
        appendUrlParam(query, "cull",
                       cfg.cull == CullOverride::ForceCull ? "force-on" : "force-off");
    }
    appendUrlBool(query, "pauseWhenUnfocused", cfg.pause_when_unfocused, true);
    appendUrlBool(query, "autoOrbit", cfg.auto_orbit, false);
    appendUrlFloat(query, "orbitSpeed", cfg.auto_orbit_speed_deg, 15.f);
    appendUrlBool(query, "angleCut", cfg.angle_cut, false);
    appendUrlBool(query, "shaderAngleCut", cfg.shader_angle_cut, true);
    appendUrlBool(query, "booleanCut", cfg.boolean_cut, false);
    appendUrlFloat(query, "cutStart", cfg.angle_cut_start_deg, 0.f);
    appendUrlFloat(query, "cutEnd", cfg.angle_cut_end_deg, 90.f);
    appendUrlBool(query, "pbr", cfg.enable_pbr, true);
    appendUrlParam(query, "cameraTargetX", formatUrlFloat(camera.target.x));
    appendUrlParam(query, "cameraTargetY", formatUrlFloat(camera.target.y));
    appendUrlParam(query, "cameraTargetZ", formatUrlFloat(camera.target.z));
    appendUrlParam(query, "cameraDistance", formatUrlFloat(camera.distance));
    appendUrlParam(query, "cameraYaw", formatUrlFloat(glm::degrees(camera.yaw)));
    appendUrlParam(query, "cameraPitch", formatUrlFloat(glm::degrees(camera.pitch)));
    if (camera.projection == ProjectionMode::Orthographic) {
        appendUrlParam(query, "cameraProjection", "orthographic");
    }
    return query;
}

void App::Impl::syncBrowserUrl() const {
    if constexpr (platform::kIsWeb) {
        const std::string state_query = browserUrlStateQuery();
        constexpr const char *kManagedKeys =
            "cull,pauseWhenUnfocused,autoOrbit,orbitSpeed,angleCut,shaderAngleCut,booleanCut,"
            "cutStart,"
            "cutEnd,"
            "pbr,cameraTargetX,cameraTargetY,cameraTargetZ,cameraDistance,cameraYaw,cameraPitch,"
            "cameraProjection";
        platform_->commitUrlState(state_query, kManagedKeys);
    }
}

void App::Impl::addProjectPath(const std::filesystem::path &path) {
    using enum ProjectDropDecision::Kind;
    if (path.empty() || project_ == nullptr) {
        return;
    }
    auto decision = project_->planAddPath(path);
    if (decision.kind == Accept) {
        project_->addPath(path);
        build_error.clear();
        return;
    }

    if (decision.kind == Confirm) {
        enqueueProjectDropModal(std::move(decision), [this, path = path]() {
            if (project_ == nullptr) {
                return;
            }
            project_->addPath(path);
            build_error.clear();
        });
    } else {
        enqueueProjectDropModal(std::move(decision), {});
    }
}

void App::Impl::addProjectBytes(const std::string &filename, std::span<const std::byte> bytes) {
    using enum ProjectDropDecision::Kind;
    if (filename.empty() || project_ == nullptr) {
        return;
    }
    auto decision = project_->planAddBytes(filename, bytes);
    if (decision.kind == Accept) {
        project_->addBytes(filename, bytes);
        build_error.clear();
        return;
    }

    if (decision.kind == Confirm) {
        enqueueProjectDropModal(std::move(decision), [this, filename = filename, bytes = bytes]() {
            if (project_ == nullptr) {
                return;
            }
            project_->addBytes(filename, std::span<const std::byte>{bytes.data(), bytes.size()});
            build_error.clear();
        });
    } else {
        enqueueProjectDropModal(std::move(decision), {});
    }
}

void App::Impl::enqueueProjectDropModal(ProjectDropDecision decision,
                                        std::function<void()> on_confirm) {
    RetainedModal modal;
    modal.title = decision.title.empty() ? "Project file drop" : std::move(decision.title);
    modal.message = std::move(decision.message);
    modal.confirm_label = decision.confirm_label.empty() ? "OK" : std::move(decision.confirm_label);
    modal.cancel_label =
        decision.cancel_label.empty() ? "Cancel" : std::move(decision.cancel_label);
    modal.show_cancel = decision.kind == ProjectDropDecision::Kind::Confirm;
    modal.on_confirm = std::move(on_confirm);
    enqueueModal(std::move(modal));
}

void App::Impl::enqueueModal(RetainedModal modal) {
    modal.id = next_modal_id++;
    active_modals.push_back(std::move(modal));
}

void App::Impl::renderActiveModal() {
    if (active_modals.empty()) {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    auto &modal = active_modals.back();
    const std::string popup_id = modal.title + "###modal_" + std::to_string(modal.id);
    if (!modal.opened) {
        ImGui::OpenPopup(popup_id.c_str());
        modal.opened = true;
    }

    const float max_width = std::min(640.f, viewport->Size.x * 0.85f);
    const float min_width = std::min(360.f, max_width);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({min_width, 0.f}, {max_width, viewport->Size.y * 0.85f});
    if (ImGui::BeginPopupModal(popup_id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", modal.message.c_str());
        ImGui::Separator();

        if (ImGui::Button(modal.confirm_label.c_str())) {
            auto on_confirm = std::move(modal.on_confirm);
            ImGui::CloseCurrentPopup();
            active_modals.pop_back();
            if (on_confirm) {
                on_confirm();
            }
            ImGui::EndPopup();
            return;
        }
        if (modal.show_cancel) {
            ImGui::SameLine();
            if (ImGui::Button(modal.cancel_label.c_str())) {
                auto on_cancel = std::move(modal.on_cancel);
                ImGui::CloseCurrentPopup();
                active_modals.pop_back();
                if (on_cancel) {
                    on_cancel();
                }
                ImGui::EndPopup();
                return;
            }
        }

        ImGui::EndPopup();
    }
}

void App::Impl::onFrame() {
    // Resolve idle-mode once per frame: shouldThrottleIdle reads
    // event-loop state (input timestamp, focus, drag hover) that
    // doesn't change during onFrame, so a single read is fine. Used
    // by the gate below to either throttle the visible frame to a
    // heartbeat (jobs running) or skip it entirely (truly idle).
    const bool idle = shouldThrottleIdle();

    // A screenshot export must keep the frame loop running at full rate (it spans
    // several render frames plus an async readback), so it bypasses every idle /
    // pause / fps-cap gate below. A *pending* startup screenshot counts too —
    // otherwise an unfocused/backgrounded window would park at the idle gate
    // before the loop ever reaches the trigger.
    const bool exporting = export_phase != ExportPhase::Idle;
    const bool keep_loop_awake = exporting || startup_screenshot_pending;

    // Auto-rebake on settings change, debounced. Each frame we check whether
    // the user has touched any IBL slider since last frame; if so, reset the
    // settle timer. Once settings have been stable for `kIblRebakeDebounce`
    // and differ from the last bake, set `ibl_rebake_pending`. Manual
    // "Rebake IBL" still works through the same flag.
    {
        const auto now = std::chrono::steady_clock::now();
        if (ibl_settings != ibl_last_seen_settings) {
            ibl_last_seen_settings = ibl_settings;
            ibl_settle_at = now + kIblRebakeDebounce;
        }
        if (ibl_installed && !ibl_rebake_pending && ibl_settings != ibl_last_baked_settings &&
            now >= ibl_settle_at) {
            ibl_rebake_pending = true;
        }
    }

    // Procedural IBL bake — runs on the GPU in the first frame, before the
    // swapchain pass that draws the scene. Same-frame ordering is fine:
    // sokol guarantees images written by an earlier pass are sampleable in
    // a later pass within the same frame.
    if (!ibl_installed || ibl_rebake_pending) {
        const bool first = !ibl_installed;
        const auto bake_start = std::chrono::steady_clock::now();
        // Bake once and install into both renderers; the IBL images are
        // reference-counted (SharedImage), so the two installs share one bake
        // and the GPU images free when the last renderer releases.
        const auto baked = bakeIblGpu(ibl_settings);
        scene_renderer.installIbl(baked);
        cut_renderer.installIbl(baked);
        const auto elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bake_start)
                .count();
        if (first) {
            std::println("viewer: IBL bake complete ({:.1f} ms)", elapsed_ms);
        } else {
            std::println("viewer: IBL rebake complete ({:.1f} ms)", elapsed_ms);
            notifications.info("IBL rebake complete");
        }
        ibl_installed = true;
        ibl_rebake_pending = false;
        ibl_last_baked_settings = ibl_settings;
        ibl_last_seen_settings = ibl_settings;
    }

    // Drive the off-loop tessellation. On native this is a poll of an
    // atomic flag set by the worker thread; on web it runs the build
    // synchronously on the second poll (the first paints a frame).
    if (build_in_progress) {
        if (build_progress_handle != 0) {
            std::string label;
            float frac = 0.0f;
            if (build_job.phase() == SceneBuildJob::Phase::Cutting) {
                // Cooperative wedge cut — bar the placement-classification sweep.
                const auto total = build_job.wedgeCutTotal();
                const auto processed = build_job.wedgeCutProcessed();
                frac = total > 0 ? static_cast<float>(processed) / static_cast<float>(total) : 0.0f;
                label = total > 0 ? std::format("Applying cut ({}/{} placements)", processed, total)
                                  : std::string{"Applying cut..."};
            } else {
                const auto total = build_job.tessellationTotal();
                const auto processed = build_job.tessellationProcessed();
                frac = total > 0 ? static_cast<float>(processed) / static_cast<float>(total) : 0.0f;
                if (total > 0) {
                    label = std::format("Tessellating ({}/{} nodes)", processed, total);
                }
            }
            notifications.updateProgress(build_progress_handle, frac, label);
        }
    }
    if (build_in_progress && build_job.poll()) {
        auto built = build_job.take();
        for (const auto &d : built.diags.items()) {
            std::println(stderr, "scene_build: {} {}", d.code, d.message);
            notifications.diagnostic(d);
        }
        if (built.scene) {
            const auto build_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - build_start_time)
                                      .count();
            std::println("viewer: tessellation complete ({:.1f} ms, {} nodes, {} mesh assets, "
                         "{} materials)",
                         build_ms, built.scene->nodes.size(), built.scene->meshAssets.size(),
                         built.scene->materials.size());
            if (build_progress_handle != 0) {
                notifications.finishProgress(build_progress_handle, "Tessellation complete");
                build_progress_handle = 0;
            }
            if (building_cut) {
                // Boolean-cut bake → cut renderer. Record the angle it was built
                // at (not the live cfg, which may have moved during the build) so
                // the render selector knows whether it's still fresh.
                cut_scene = std::move(built.scene);
                cut_uploaded = false;
                cut_built_start_deg = in_flight_cut_start_deg;
                cut_built_end_deg = in_flight_cut_end_deg;
            } else {
                scene = std::move(built.scene);
                scene_uploaded = false;
                camera_framed = false;
                // The freshly loaded base scene needs a cut bake if the Boolean
                // cut is already enabled (e.g. from persisted state / URL).
                if (cfg.boolean_cut) {
                    pending_cut_rebuild = true;
                }
            }
            build_error.clear();
        } else {
            // Errors are already surfaced as toasts via diagnostic() above;
            // stash the first one for the persistent status-bar message.
            build_error = "scene build failed";
            for (const auto &d : built.diags.items()) {
                if (d.severity >= DiagnosticSeverity::Error) {
                    build_error = d.message;
                    break;
                }
            }
            if (build_progress_handle != 0) {
                notifications.cancelProgress(build_progress_handle);
                build_progress_handle = 0;
            }
        }
        build_in_progress = false;
        // Project is long-lived: we keep it so additional drops/picks
        // accumulate into the existing bag (or so a UrlProjectFs's state
        // survives in case the user wants to inspect what loaded). The
        // build job already has the paths it needs; nothing else to do.
    }

    // Idle gate, with a dynamic cadence based on whether there's
    // anything that wants visible progress. Two cases:
    //
    //  • Jobs in flight (bake / build / GPU upload / config walk):
    //    redraw at ~30 Hz so progress toasts and the build → render
    //    transition update smoothly. The IBL/build polls already
    //    ran above with their default per-call budget; this gate
    //    only governs the visible frame.
    //
    //  • Nothing pending: skip the visible frame entirely. On a
    //    heavy ODD scene the render pass dominates idle GPU usage
    //    on native — this gives effectively 0% GPU when the viewer
    //    is parked in the background and there's nothing to show.
    //
    // Anything that wants full rate (input, drag hover, future
    // animations) bypasses this gate via shouldThrottleIdle. Field/
    // URL/CLI keep the `pauseWhenUnfocused` name for backwards
    // compatibility even though the rule no longer keys on focus
    // alone and the cadence is now dynamic.
    if (idle && !keep_loop_awake) {
        const auto session_phase = build_session.phase();
        const bool jobs_running = !ibl_installed || build_in_progress ||
                                  (scene && !scene_uploaded) || (cut_scene && !cut_uploaded) ||
                                  pending_cut_rebuild || session_phase == BuildPhase::Walking ||
                                  session_phase == BuildPhase::ResolvedReady;
        if (!jobs_running) {
            return;
        }
        constexpr double kIdleFrameInterval = 1.0 / 30.0; // 30 Hz
        if (stm_sec(stm_diff(stm_now(), last_time)) < kIdleFrameInterval) {
            return;
        }
    }

    // Focused idle frame-rate cap. Once nothing is actively changing, the only
    // per-frame work is the cheap composite + ImGui — the scene is cached and
    // rendered on demand (see render_scene below) — so vsync is overkill. Cap to
    // a low rate to trim idle power while keeping the UI live (and self-updating,
    // so there's no stale-frame risk). Recent input, running jobs, a pending
    // rebake, auto-orbit, a file-drag, and live toasts all bypass the cap so
    // interaction and animations stay at the full refresh rate; input bypasses
    // it on the very next vsync, so first-interaction latency is unaffected.
    // (This is the focused counterpart to the unfocused idle gate above; that
    // one skips entirely / 30 Hz when backgrounded.)
    if (quality.pause_when_static && !keep_loop_awake) {
        constexpr double kIdleInputSeconds = 0.2;         // recent input keeps full rate
        constexpr double kIdleFrameInterval = 1.0 / 12.0; // ~12 Hz when parked
        const auto session_phase = build_session.phase();
        const bool jobs_running = !ibl_installed || build_in_progress ||
                                  (scene && !scene_uploaded) || (cut_scene && !cut_uploaded) ||
                                  pending_cut_rebuild || session_phase == BuildPhase::Walking ||
                                  session_phase == BuildPhase::ResolvedReady;
        const bool ibl_dirty = ibl_rebake_pending || ibl_settings != ibl_last_baked_settings;
        const bool drag_hover = platform_ && platform_->windowState().drag_hover.active;
        // Trackpad pinch-zoom arrives via the platform gesture queue, not as a
        // sokol input event, so it never bumps last_activity_time. Peek the
        // queue here so an in-flight gesture keeps full rate from its very first
        // frame; consuming the events below also bumps the activity clock, which
        // carries the full rate through the brief settle tail after it ends.
        const bool pending_gesture = platform_ && platform_->hasPendingGestures();
        const bool input_recent =
            last_activity_time != 0 &&
            stm_sec(stm_diff(stm_now(), last_activity_time)) < kIdleInputSeconds;
        const bool full_rate = input_recent || jobs_running || ibl_dirty || cfg.auto_orbit ||
                               drag_hover || pending_gesture || notifications.hasActiveToasts();
        if (!full_rate && last_time != 0 &&
            stm_sec(stm_diff(stm_now(), last_time)) < kIdleFrameInterval) {
            return;
        }
    }

    // Opt-in 60 FPS cap. sokol drives this callback at the display's vsync
    // rate, so on a high-refresh panel (120Hz+) skip the frames that would
    // push us past 60. The slack keeps a true 60Hz vsync — which may report a
    // hair under 16.67ms from timer jitter — from being mistaken as "too
    // early" and dropped to 30. When idle this never bites: the idle gate
    // above already throttles below 60.
    if (quality.cap_fps && !keep_loop_awake) {
        constexpr double kFpsCapInterval = 1.0 / 60.0;
        constexpr double kFpsCapSlack = kFpsCapInterval * 0.1;
        if (stm_sec(stm_diff(stm_now(), last_time)) < kFpsCapInterval - kFpsCapSlack) {
            return;
        }
    }

    fb_width = static_cast<uint32_t>(sapp_width());
    fb_height = static_cast<uint32_t>(sapp_height());

    delta_seconds = stm_sec(stm_laptime(&last_time));
    frame_interval_ms = delta_seconds * 1000.0;

    // FPS window: rolling once per second so the number is readable.
    ++frame_count;
    if (fps_window_start == 0) {
        fps_window_start = last_time;
    } else {
        const double window_secs = stm_sec(stm_diff(last_time, fps_window_start));
        if (window_secs >= 1.0) {
            fps = static_cast<float>(frame_count) / static_cast<float>(window_secs);
            frame_count = 0;
            fps_window_start = last_time;
        }
    }

    // Sample the rolling timing history once per rendered frame. The submit
    // timings still hold last frame's values here (they're measured during
    // render() further down), which is harmless for a scrolling graph.
    perf_history.push(delta_seconds, frame_interval_ms, encode_ms, present_ms, gpu_wait_ms,
                      scene_submit_ms, fps);

    // simgui_new_frame internally calls ImGui::NewFrame after configuring
    // io display size + delta time. Don't double-call NewFrame.
    ImGui_ImplSokol_NewFrame(static_cast<int>(fb_width), static_cast<int>(fb_height), delta_seconds,
                             sapp_dpi_scale());

    platform_->beginFrameWindowSync();
    platform_window_state = platform_->windowState();
    platform_gesture_events = platform_->takeGestureEvents();
    if (!platform_gesture_events.empty()) {
        // A platform gesture (trackpad pinch-zoom) is a user-input-class event
        // just like a scroll, but it bypasses onEvent — bump the activity clock
        // here so the idle gate keeps full rate through the settle tail.
        last_activity_time = stm_now();
    }

    updateCameraInput();
    if (scene && cfg.auto_orbit) {
        camera.orbit(glm::radians(cfg.auto_orbit_speed_deg) * static_cast<float>(delta_seconds),
                     0.f);
    }

    // Drive the project + build pipeline unconditionally — running this
    // only when `!scene` means double-clicking a different config in the
    // tree panel after a scene is already rendered would update the
    // session's root keys but never actually walk → parse → build the
    // new selection. The build-job completion above swaps `scene` over
    // when the new build lands.
    // Kick off a build. `wedge` is nullopt for the uncut base scene and set for
    // a Boolean-cut bake; `building_cut` records which so completion routes the
    // result to `scene` vs `cut_scene`. Inputs come in as shared_ptr<const> so
    // both callers (a fresh BuildSession build and a re-aimed cut) just refcount
    // here — the build job takes the deep copy on its worker thread.
    auto start_build = [this](std::shared_ptr<const ::nodehammer::NHConfig> config,
                              std::shared_ptr<const ::nodehammer::SemanticScene> semantic_scene,
                              std::string config_label, std::string geometry_label,
                              std::optional<::nodehammer::WedgeCutParams> wedge) {
        build_start_time = std::chrono::steady_clock::now();
        building_cut = wedge.has_value();
        if (building_cut) {
            in_flight_cut_start_deg = cfg.angle_cut_start_deg;
            in_flight_cut_end_deg = cfg.angle_cut_end_deg;
        }
        build_job.start(std::move(config), std::move(semantic_scene), std::move(config_label),
                        std::move(geometry_label), wedge);
        build_in_progress = true;
        build_progress_handle =
            notifications.startProgress(building_cut ? "Applying cut..." : "Tessellating...");
    };

    if (project_) {
        project_->poll();
        build_session.poll(project_.get());

        if (!build_in_progress && build_session.phase() == BuildPhase::ResolvedReady) {
            if (auto inputs = build_session.takeInputs()) {
                // Cache pristine (uncut) inputs so cut bakes re-derive cleanly.
                // Held as shared_ptr<const> and handed straight to the build job,
                // so the move out of `inputs` is the only copy and the cut path
                // later re-uses these without re-walking the project.
                pristine_config = std::make_shared<const ::nodehammer::NHConfig>(
                    std::move(inputs->config.config));
                pristine_scene = std::make_shared<const ::nodehammer::SemanticScene>(
                    std::move(inputs->import.scene));
                pristine_config_label = std::move(inputs->config_key);
                pristine_geometry_label = std::move(inputs->geometry_key);
                // A new base build invalidates any resident cut bake.
                cut_scene.reset();
                cut_uploaded = false;
                cut_renderer.clearScene();
                pending_cut_rebuild = false;
                // The base scene is always uncut (wedge = nullopt); the cut bake
                // follows once the base lands (see completion handler).
                start_build(pristine_config, pristine_scene, pristine_config_label,
                            pristine_geometry_label, std::nullopt);
            }
        }
    }

    // Boolean-cut (re)build: re-prep + re-tessellate the wedge cut from the
    // cached pristine scene (never from already-cut geometry). Skipped if a
    // resident cut already matches the committed angle.
    const bool cut_fresh = cut_uploaded && cut_built_start_deg == cfg.angle_cut_start_deg &&
                           cut_built_end_deg == cfg.angle_cut_end_deg;
    if (pending_cut_rebuild && !build_in_progress && pristine_scene) {
        pending_cut_rebuild = false;
        if (cfg.boolean_cut && !cut_fresh) {
            // Hand the cached pristine inputs straight through (refcount bump);
            // the worker thread takes the scene copy that prep consumes, so the
            // frame that locks the angle no longer stalls on the deep copy.
            start_build(
                pristine_config, pristine_scene, pristine_config_label, pristine_geometry_label,
                ::nodehammer::WedgeCutParams{cfg.angle_cut_start_deg, cfg.angle_cut_end_deg});
        }
    }

    ui::ViewerUiContext ui_ctx{
        .cfg = cfg,
        .quality = quality,
        .export_settings = export_settings,
        .project = project_.get(),
        .build_session = build_session,
        .build_job = build_job,
        // Stats reflect the renderer drawn last frame (base or cut). active_
        // renderer is updated in render(); a one-frame lag here is harmless.
        .scene_renderer = (active_renderer != nullptr) ? *active_renderer : scene_renderer,
        .camera = camera,
        .notifications = &notifications,
        .platform_window_state = platform_window_state,
        .root_config_key = root_config_key,
        .root_geometry_key = root_geometry_key,
        .build_error = build_error,
        .fb_width = fb_width,
        .fb_height = fb_height,
        .scene_width = scene_rt.width,
        .scene_height = scene_rt.height,
        .ao_width = ao_hist[ao_hist_last_written].width,
        .ao_height = ao_hist[ao_hist_last_written].height,
        .fps = fps,
        .frame_interval_ms = frame_interval_ms,
        .render_submit_ms = render_submit_ms,
        .encode_ms = encode_ms,
        .present_ms = present_ms,
        .gpu_wait_ms = gpu_wait_ms,
        .scene_submit_ms = scene_submit_ms,
        .call_stats =
            {
                .num_apply_pipeline = render_stats.num_apply_pipeline,
                .num_apply_bindings = render_stats.num_apply_bindings,
                .num_apply_uniforms = render_stats.num_apply_uniforms,
                .num_draw = render_stats.num_draw,
                .mtl_set_render_pipeline_state =
                    render_stats.metal.pipeline.num_set_render_pipeline_state,
                .mtl_set_vertex_buffer = render_stats.metal.bindings.num_set_vertex_buffer,
                .mtl_skip_vertex_buffer =
                    render_stats.metal.bindings.num_skip_redundant_vertex_buffer,
            },
        .perf_history = &perf_history,
        .has_scene = static_cast<bool>(scene),
        .scene_uploaded = scene_uploaded,
        .build_in_progress = build_in_progress,
        .export_in_progress = exporting,
        .ibl_installed = ibl_installed,
        .hdr_supported = hdrSupported(),
        .ibl_settings = &ibl_settings,
    };

    ui::UiActions ui_actions;
    ui_actions.sync_browser_url = [this]() { syncBrowserUrl(); };
    ui_actions.open_url = [this](const std::string &url) { platform_->openUrl(url); };
    ui_actions.rebake_ibl = [this]() { ibl_rebake_pending = true; };
    ui_actions.request_scene_rebuild = [this]() { pending_cut_rebuild = true; };
    ui_actions.export_png = [this]() { requestPngExport(export_settings); };
    ui_actions.open_file_picker = [this]() { platform_->openFilePicker(); };
    ui_actions.open_folder_picker = [this]() { platform_->openFolderPicker(); };
    ui_actions.frame_scene = [this]() {
        if (!scene) {
            return;
        }
        glm::vec3 bmin{0.f}, bmax{0.f};
        if (scene_renderer.worldBounds(bmin, bmax)) {
            scene_radius = camera.frameBounds(bmin, bmax);
        }
    };
    ui_actions.close_project = [this]() {
        scene_renderer.clearScene();
        cut_renderer.clearScene();
        scene.reset();
        cut_scene.reset();
        pristine_scene.reset();
        project_ = platform::makeEmptyBag();
        project_->setLogSink(&notifications);
        root_config_key.clear();
        root_geometry_key.clear();
        build_session.setRootKeys({}, {});
        active_modals.clear();
        scene_uploaded = false;
        cut_uploaded = false;
        pending_cut_rebuild = false;
        camera_framed = false;
        scene_radius = 0.f;
        build_error.clear();
        notifications.info("Project closed");
    };
    ui_actions.rescan_project = [this]() {
        if (project_) {
            project_->rescan();
            build_error.clear();
            notifications.info("Project rescan requested");
        }
    };
    ui_actions.select_config_key = [this](std::string key) {
        root_config_key = std::move(key);
        build_session.setRootKeys(root_config_key, root_geometry_key);
        build_error.clear();
    };
    ui_actions.select_geometry_key = [this](std::string key) {
        root_geometry_key = std::move(key);
        build_session.setRootKeys(root_config_key, root_geometry_key);
        build_error.clear();
    };

    ui::renderViewerUi(ui_state, ui_ctx, ui_actions);
    renderActiveModal();
    notifications.render();

    // Headless screenshot: once the scene is loaded, uploaded, lit and framed,
    // fire the one-shot export (which quits when the file is written).
    if (startup_screenshot_pending && export_phase == ExportPhase::Idle && scene &&
        scene_uploaded && ibl_installed && camera_framed && !build_in_progress) {
        startup_screenshot_pending = false;
        requestPngExport(startup_screenshot_settings, startup_screenshot_path,
                         /*quit_when_done=*/true);
    }

    // Set up any export-frame state (target allocation, dims/quality override
    // flags) before render() reads it; advance the export state machine after.
    exportPreRender();

    // simgui_render (called from inside render() → ImGui_ImplSokol_Render)
    // internally calls ImGui::Render itself before issuing draws.
    render();

    exportPostRender();

    // Drain any pending native picker modal. ImGui frame is ended,
    // sokol pass is committed; if NFD spawns a nested run loop and
    // re-enters frame_cb, it'll hit a clean self-contained frame
    // instead of overlapping with one already in progress. Web's
    // pickers dispatch inline at click-time, so this is a no-op
    // there — both platforms are reached through the same call.
    platform_->drainPickers();

    savePersistentState(false);
}

void App::Impl::onCleanup() {
    savePersistentState(true);
    composite.release();
    ao_denoise_pass.release();
    ao_pass.release();
    ao_hist[0].destroy();
    ao_hist[1].destroy();
    ao_rt_raw.destroy();
    export_out_rt.destroy();
    export_readback.reset();
    scene_rt.destroy();
    scene_renderer.release();
    cut_renderer.release();
    // simgui_shutdown destroys the ImGui context — don't call
    // ImGui::DestroyContext separately (double-free crash on macOS quit).
    ImGui_ImplSokol_Shutdown();
    sg_shutdown();
}

void App::setScene(std::shared_ptr<const RenderScene> scene) {
    impl_->scene = std::move(scene);
    impl_->scene_uploaded = false;
    impl_->camera_framed = false;
}

void App::requestScreenshot(std::string path, PngExportSettings settings) {
    impl_->startup_screenshot_pending = true;
    impl_->startup_screenshot_path = std::move(path);
    impl_->startup_screenshot_settings = settings;
}

void App::setProject(std::unique_ptr<ProjectFs> project) {
    impl_->project_ = std::move(project);
    if (impl_->project_) {
        impl_->project_->setLogSink(&impl_->notifications);
    }
    // Reset session-side state so the BuildSession doesn't keep walking
    // an old project's stale keys against a new backend. Root keys
    // either come back via setRootKeys (URL mode) or via App-side
    // recognition on the next generation bump (bag mode).
    impl_->root_config_key.clear();
    impl_->root_geometry_key.clear();
    impl_->build_session.setRootKeys({}, {});
    impl_->active_modals.clear();
}

void App::setRootKeys(std::string config_key, std::string geometry_key) {
    impl_->root_config_key = config_key;
    impl_->root_geometry_key = geometry_key;
    impl_->build_session.setRootKeys(std::move(config_key), std::move(geometry_key));
}

ProjectFs *App::project() const noexcept { return impl_->project_.get(); }

void App::addProjectPath(const std::filesystem::path &path) { impl_->addProjectPath(path); }

void App::addProjectBytes(const std::string &filename, std::span<const std::byte> bytes) {
    impl_->addProjectBytes(filename, bytes);
}

void App::savePersistentState() { impl_->savePersistentState(true); }

namespace {
// Function-local-static slot that owns the singleton App. `App::Handle`
// constructs into the slot; on native its dtor resets the slot, on web
// it leaves the App in place so sokol's frame callbacks (which fire from
// the JS event queue after main returns) keep dispatching against a live
// instance. Either way the static unique_ptr's own destructor runs at
// program exit and is the eventual teardown.
std::unique_ptr<App> &slot() {
    static std::unique_ptr<App> p;
    return p;
}
} // namespace

App *App::instance() { return slot().get(); }

App::App(PrivateTag /*tag*/, Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {
    // Construct Platform after the App body is in place so it can
    // hold a back-reference to *this. The Platform pImpl is defined
    // per-TU (platform_macos.mm / platform_native_default.cpp / platform_web.cpp); whichever TU
    // is linked into this executable supplies the matching ctor.
    impl_->platform_ = std::make_unique<platform::Platform>(*this);
}

App::~App() = default;

App::Handle::Handle(Config cfg) {
    if (slot()) {
        throw std::logic_error("App::Handle constructed while another is alive");
    }
    slot() = std::make_unique<App>(App::PrivateTag{}, std::move(cfg));
}

App::Handle::~Handle() {
    if constexpr (!platform::kIsWeb) {
        slot().reset();
    }
    // Web: leave the App alive in the slot. The static unique_ptr's own
    // dtor (program exit) is the eventual teardown — necessary because
    // sapp_run returned immediately after registering the main loop and
    // sokol will keep firing frame callbacks against impl_ from the JS
    // event queue.
}

App *App::Handle::operator->() const noexcept { return slot().get(); }
App &App::Handle::operator*() const noexcept { return *slot(); }

int App::run() {
    sapp_desc desc{};
    desc.user_data = impl_.get();
    desc.init_userdata_cb = &Impl::initCb;
    desc.frame_userdata_cb = &Impl::frameCb;
    desc.event_userdata_cb = &Impl::eventCb;
    desc.cleanup_userdata_cb = &Impl::cleanupCb;
    desc.width = static_cast<int>(impl_->cfg.width);
    desc.height = static_cast<int>(impl_->cfg.height);
    desc.window_title = impl_->cfg.title.c_str();
    desc.high_dpi = true;
    desc.swap_interval = impl_->cfg.vsync ? 1 : 0;
    // Default canvas selector for emscripten: matches the <canvas id="canvas">
    // in web/viewer.html.
    desc.html5.canvas_selector = "#canvas";
    desc.logger.func = slog_func;
    // Enable drag-and-drop file events. The buffer size needs to be at
    // least the longest path we'd ever see; 1 KiB is safe for typical
    // scene+config drops.
    desc.enable_dragndrop = true;
    desc.max_dropped_files = 8;
    desc.max_dropped_file_path_length = 1024;
    impl_->platform_->configureWindowDesc(desc, impl_->cfg, impl_->window_customization);
    // sapp_run on emscripten registers the main loop and returns (older
    // sokol versions did a stack-unwind, current ones don't). The Impl
    // outlives the unwind anyway because App lives in the static slot()
    // and impl_ is its member — both heap-resident, both survive stack
    // unwinding. Don't release impl_ here: doing so leaves
    // App::project() / App::setProject() (which dereference impl_)
    // pointing at a null unique_ptr, which silently returns 0 on web.
    sapp_run(&desc);
    return 0;
}

} // namespace nodehammer::viewer

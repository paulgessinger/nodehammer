#include <nodehammer/viewer/app.hpp>

#include "ao_denoise_pass.hpp"
#include "ao_pass.hpp"
#include "ao_render_target.hpp"
#include "bench_runner.hpp"
#include "blit_pass.hpp"
#include "build_controller.hpp"
#include "composite_pass.hpp"
#include "gpu_pass_timer.hpp"
#include "ibl.hpp"
#include "ibl_baker.hpp"
#include "imgui_backend.hpp"
#include "png_export_readback.hpp"
#include "png_exporter.hpp"
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
#include <nodehammer/viewer/dynamic_render_scale.hpp>
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
#include <cstdlib>
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
    // The composite + ImGui render into this persistent window-sized target
    // instead of straight to the swapchain; blit_pass_ then copies it into the
    // swapchain. "Pause when static" re-presents the cached frame with just the
    // blit (see presentCachedFrame) — cheaper than a full re-render, and it keeps
    // sokol's unconditional D3D11 Present() from scanning out a stale flip-model
    // back buffer on skipped frames (which read as the perf plot going backward).
    SceneRenderTarget present_cache_;
    BlitPass blit_pass_;
    // Per-pass GPU timestamps (real timing on D3D11, no-op elsewhere). Surfaces
    // the GPU cost the CPU submit timers can't see — see gpu_pass_timer.hpp.
    GpuPassTimer gpu_pass_timer_;
    RenderQualitySettings quality;
    Camera camera;

    // ── PNG screenshot export ────────────────────────────────────────────────
    // High-res export: render the scene at the export resolution with every
    // quality knob maxed, composite into a dedicated LDR target, read it back,
    // box-downscale to the requested output size, then write (native) or
    // download (web) a PNG. The whole Idle→Rendering→WaitGpu→Readback machine
    // lives in PngExporter now; render() consults its flags/target each frame,
    // and onFrame drives it via preRender()/postRender(). `export_settings` is
    // the live, UI-edited output size/supersample (the exporter snapshots +
    // clamps it at request time).
    PngExportSettings export_settings;
    PngExporter exporter_;
    // Pending one-shot startup screenshot (set by App::requestScreenshot before
    // run()). Triggered from onFrame once the scene is loaded and settled.
    bool startup_screenshot_pending{false};
    std::string startup_screenshot_path;
    PngExportSettings startup_screenshot_settings;

    // Headless benchmark mode (set by App::requestBench before run()). When
    // active, onFrame bypasses the UI/input/idle path and drives `bench_` through
    // a fixed camera/state sequence. The capture members mirror the exporter's
    // grab-then-readback, but at window resolution and the live (not maxed)
    // quality — "the current frame", per the bench design.
    bool bench_pending_{false};
    std::string bench_json_path_;
    std::string bench_scene_label_;
    float bench_render_scale_{1.0f}; // SSAA on the scene/AO passes — push GPU-bound for 4K-equiv
    std::unique_ptr<BenchRunner> bench_;
    std::string bench_capture_request_; // non-empty: composite+read back to this path this frame
    bool bench_capture_issued_{false};  // the grab composite ran in render() this frame
    bool bench_capture_done_{false};    // a capture finished writing since the last bench update
    // Readback runs as a small cross-frame state machine (like PngExporter): the
    // grab composite is issued, a few frames drain the GPU, then the readback is
    // polled until Ready. Doing begin+poll in one frame only works on D3D11's
    // synchronous Map — WebGPU/Metal need to wait, or the PNG is stale/missing.
    enum class BenchCapturePhase { Idle, WaitGpu, Readback };
    BenchCapturePhase bench_capture_phase_{BenchCapturePhase::Idle};
    int bench_capture_wait_{0};          // frames left to drain before readback
    bool bench_readback_started_{false}; // ImageReadback::begin() has been called
    SceneRenderTarget bench_grab_rt_;
    ImageReadback bench_readback_;
    std::uint64_t bench_wait_frames_{0};
    void benchInit();
    void benchFrame();
    void benchCaptureReadback();
    void benchFinishCapture();

    // Procedural IBL bake. Runs on the GPU in a single frame the first time
    // `onFrame` ticks; until then `scene_renderer` samples from 1×1 dummy
    // textures created by `IblResources::createDummy()`. `ibl_settings` is the
    // live, UI-edited tunable (also read by render() for the sun direction); the
    // debounce/dirty/installed state and the bake loop live in `ibl_baker_`,
    // which onFrame drives once per tick. The "Rebake IBL" action and any slider
    // edit flow through the baker (a bake pass cannot be issued from inside the
    // swapchain pass that hosts the UI draw, so it runs at the top of onFrame).
    IblSettings ibl_settings{};
    IblBaker ibl_baker_;

    // CPU-side build orchestration: the BuildSession (walk → parse → import),
    // the off-loop SceneBuildJob (validate → select → dedup → wedge →
    // tessellate), the pristine-inputs cache, the Boolean-cut bookkeeping, the
    // build-progress toast, the root keys, and the persistent error string all
    // live in the controller now. It emits CPU RenderScenes via callbacks the
    // App binds to its GPU/scene state (see onInit). render() reads its
    // cut-built angles; onFrame drives it once per frame. `cut_uploaded` and
    // `last_shown_cut` stay here because they track the App's GPU upload of the
    // cut scene, which the controller doesn't own.
    BuildController build_controller_;
    bool cut_uploaded{false};   ///< whether cut_scene is GPU-resident
    bool last_shown_cut{false}; ///< whether last frame drew the cut scene (AO flip reset)

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

    // Adaptive render-scale controller. When quality.dynamic_render_scale is on,
    // the offscreen scale drops while the camera moves (to protect framerate)
    // and jumps back up to render_scale_max once it settles; the EMA/lock state
    // and the memory-budget clamp all live inside the controller now. render()
    // feeds it a camera snapshot + per-frame timing and applies the returned
    // scale. (See DynamicRenderScale.)
    DynamicRenderScale dyn_scale_;

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
    // Whether the cached scene_rt color holds an overdraw count (vs a shaded
    // beauty frame). The overdraw debug view replaces the scene color content,
    // so toggling it on or off must force one scene re-render even under
    // pause_when_static -- unlike the depth views, which read cached depth and
    // leave the color untouched. Compared against the current debug view each
    // frame to detect the transition.
    bool last_rendered_overdraw{false};

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

    // True while any async work that mutates the rendered scene is in flight:
    // the IBL bake, a geometry build, an un-uploaded scene/cut, or a pending
    // cut rebuild. The frame loop consults it to decide whether it may idle,
    // throttle to a low rate, or hold a cached still. Computed identically at
    // three sites in render()/onFrame(); centralized here.
    [[nodiscard]] bool jobsRunning() const {
        const auto session_phase = build_controller_.session().phase();
        return !ibl_baker_.installed() || build_controller_.inProgress() ||
               (scene && !scene_uploaded) || (cut_scene && !cut_uploaded) ||
               build_controller_.pendingCutRebuild() || session_phase == BuildPhase::Walking ||
               session_phase == BuildPhase::ResolvedReady;
    }

    void updateCameraInput();
    void applyInitialCamera();
    void render();
    // Present-cache helpers. ensurePresentCache (re)allocates present_cache_ to
    // the window size + swapchain formats; blitPresentCacheToSwapchain copies it
    // into the current frame's swapchain; presentCachedFrame issues a complete
    // cheap frame (blit + commit) that re-presents the last full frame — used by
    // the throttle gates instead of an early return so D3D11 never presents a
    // stale flip-model buffer. Returns false (does nothing) if no valid cache
    // exists yet or the window size changed, so the caller renders normally.
    void ensurePresentCache();
    void blitPresentCacheToSwapchain();
    [[nodiscard]] bool presentCachedFrame();
    // Per-pixel byte cost of the resolution-scaling offscreen targets (scene
    // color + depth, plus the AO targets when AO is on), derived from the live
    // backend pixel formats. Fed to DynamicRenderScale, which owns the
    // memory-budget clamp arithmetic (the GPU-format part stays here; the pure
    // math moved into the controller).
    [[nodiscard]] float sceneTargetBytesPerScenePx() const;
    void ensureSceneTarget(uint32_t width, uint32_t height);
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

    // Wire the IBL bake: the baker owns the debounce/dirty/installed state and
    // decides *when* to bake; the GPU bake + install into both renderers + the
    // first/rebake toast live here (the App owns the GPU + notification surface).
    // The IBL images are reference-counted (SharedImage), so the two installs
    // share one bake and free when the last renderer releases.
    ibl_baker_.setBake([this](const IblSettings &settings, bool first) {
        const auto bake_start = std::chrono::steady_clock::now();
        const auto baked = bakeIblGpu(settings);
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
    });

    // Wire the PNG exporter: it owns the export state machine; the App owns the
    // notification surface, the screenshot filename, the HDR capability query,
    // the platform save (native cwd / web download), and the quit-on-done hook.
    exporter_.configure(PngExporter::Deps{
        .notifications = &notifications,
        .make_filename = [] { return makeScreenshotFilename(); },
        .hdr_supported = [] { return hdrSupported(); },
        .save_image =
            [this](const std::string &filename, std::span<const std::byte> bytes) {
                return platform_->saveExportedImage(filename, bytes);
            },
        .on_quit = [] { sapp_quit(); },
    });

    // Wire the build controller: it owns the CPU build orchestration and hands
    // finished scenes back through these callbacks. The App binds them to its
    // GPU/scene state — GPU uploads still happen in render().
    build_controller_.configure(&notifications,
                                BuildController::Callbacks{
                                    .on_base_scene_ready =
                                        [this](std::shared_ptr<const RenderScene> s) {
                                            scene = std::move(s);
                                            scene_uploaded = false;
                                            camera_framed = false;
                                        },
                                    .on_cut_scene_ready =
                                        [this](std::shared_ptr<const RenderScene> s) {
                                            cut_scene = std::move(s);
                                            cut_uploaded = false;
                                        },
                                    .on_project_build_starting =
                                        [this] {
                                            // A new base build invalidates any
                                            // resident cut bake.
                                            cut_scene.reset();
                                            cut_uploaded = false;
                                            cut_renderer.clearScene();
                                        },
                                });
    composite.initialize();
    blit_pass_.initialize();

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
    build_controller_.session().setLogSink(&notifications);

    fb_width = static_cast<uint32_t>(sapp_width());
    fb_height = static_cast<uint32_t>(sapp_height());
    platform_->attachWindow(window_customization);

    benchInit();
}

void App::Impl::benchInit() {
    if (!bench_pending_) {
        return;
    }
    // Determinism, asserted after persisted config/quality has loaded (it could
    // otherwise clobber these). No idle/throttle/cap, fixed resolution, cut on for
    // the first build. vsync is already off (forced in requestBench before run()).
    cfg.vsync = false;
    cfg.pause_when_unfocused = false;
    cfg.auto_orbit = false;
    cfg.boolean_cut = true;
    quality.dynamic_render_scale = false;
    quality.render_scale = std::clamp(bench_render_scale_, 0.25f, 4.0f);
    quality.cap_fps = false;
    quality.pause_when_static = false;

    std::string temp_dir;
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        temp_dir = tmp.string();
    }
    bench_ = std::make_unique<BenchRunner>(camera, cfg, quality, bench_json_path_,
                                           bench_scene_label_, temp_dir);
    std::println("viewer: benchmark mode -- sequence will run once the scene settles");
}

void App::Impl::benchFrame() {
    fb_width = static_cast<uint32_t>(sapp_width());
    fb_height = static_cast<uint32_t>(sapp_height());
    delta_seconds = stm_sec(stm_laptime(&last_time));
    frame_interval_ms = delta_seconds * 1000.0;

    // Empty ImGui frame so render()'s swapchain pass (which draws ImGui) still has
    // valid draw data — the bench shows no panels.
    ImGui_ImplSokol_NewFrame(static_cast<int>(fb_width), static_cast<int>(fb_height), delta_seconds,
                             sapp_dpi_scale());

    // The runner reads the *previous* frame's results (GPU timers + stats lag one
    // frame by construction — exactly what a measure window accumulates).
    BenchFrameInput in;
    in.settled = scene_uploaded && !jobsRunning();
    glm::vec3 bmin{0.f}, bmax{0.f};
    // Base-scene bounds: stable across the cut-on/cut-off laps, so the framed pose
    // is identical for the A/B.
    in.has_bounds = scene_renderer.worldBounds(bmin, bmax);
    in.bounds_min = bmin;
    in.bounds_max = bmax;
    in.gpu = gpu_pass_timer_.results();
    const SceneRenderer::FrameStats fs =
        (active_renderer != nullptr ? active_renderer : &scene_renderer)->lastFrameStats();
    in.stats = BenchSceneStats{fs.draw_calls, fs.instances, fs.triangles};
    in.frame_ms = frame_interval_ms;
    in.cpu_submit_ms = encode_ms + present_ms;
    in.capture_done = bench_capture_done_;
    bench_capture_done_ = false;

    if (!in.settled && (bench_wait_frames_++ % 120) == 0) {
        std::println(stderr,
                     "viewer: bench waiting to settle -- session_phase={} ibl={} build={} scene={} "
                     "uploaded={} pending_cut={} err='{}'",
                     static_cast<int>(build_controller_.session().phase()), ibl_baker_.installed(),
                     build_controller_.inProgress(), static_cast<bool>(scene), scene_uploaded,
                     build_controller_.pendingCutRebuild(), build_controller_.error());
    }

    const BenchFrameOutput out = bench_->update(in);
    if (out.request_capture && bench_capture_request_.empty()) {
        bench_capture_request_ = out.capture_path;
    }

    render();
    benchCaptureReadback();

    if (out.finished) {
        const char *backend = "?";
        switch (sg_query_backend()) {
        case SG_BACKEND_D3D11:
            backend = "D3D11";
            break;
        case SG_BACKEND_GLCORE:
            backend = "GL";
            break;
        case SG_BACKEND_GLES3:
            backend = "GLES3";
            break;
        case SG_BACKEND_METAL_MACOS:
        case SG_BACKEND_METAL_IOS:
        case SG_BACKEND_METAL_SIMULATOR:
            backend = "Metal";
            break;
        case SG_BACKEND_WGPU:
            backend = "WebGPU";
            break;
        default:
            break;
        }
        bench_->writeResults(backend, fb_width, fb_height);
        bench_.reset();
        sapp_quit();
    }
}

void App::Impl::benchFinishCapture() {
    // Release the in-flight capture and tell the runner to advance. Called on
    // success and on every hard failure so a readback hiccup can't wedge the
    // sequence.
    bench_readback_.reset();
    bench_capture_request_.clear();
    bench_capture_issued_ = false;
    bench_readback_started_ = false;
    bench_capture_phase_ = BenchCapturePhase::Idle;
    bench_capture_done_ = true;
}

void App::Impl::benchCaptureReadback() {
    if (!bench_capture_issued_) {
        return;
    }
    // render() composited the captured frame into bench_grab_rt_. Read it back as
    // a cross-frame state machine so async backends (WebGPU/Metal) get the frames
    // they need — begin+poll in a single frame only works on D3D11's blocking Map.
    switch (bench_capture_phase_) {
    case BenchCapturePhase::Idle:
        // Composite was issued this frame; let a few normal frames drain the GPU
        // so the capture pass has completed before we map it (mirrors PngExporter).
        bench_capture_phase_ = BenchCapturePhase::WaitGpu;
        bench_capture_wait_ = 3;
        return;
    case BenchCapturePhase::WaitGpu:
        if (--bench_capture_wait_ > 0) {
            return;
        }
        bench_capture_phase_ = BenchCapturePhase::Readback;
        bench_readback_started_ = false;
        return;
    case BenchCapturePhase::Readback:
        break;
    }

    if (bench_grab_rt_.color.id == SG_INVALID_ID) {
        benchFinishCapture();
        return;
    }
    if (!bench_readback_started_) {
        bench_readback_started_ = true;
        if (!bench_readback_.begin(bench_grab_rt_.color, bench_grab_rt_.width,
                                   bench_grab_rt_.height, bench_grab_rt_.color_format)) {
            std::println(stderr, "viewer: bench screenshot readback unsupported on this backend");
            benchFinishCapture();
            return;
        }
    }
    std::vector<std::uint8_t> pixels;
    const ReadbackStatus st = bench_readback_.poll(pixels);
    if (st == ReadbackStatus::Pending) {
        return; // async backend not done yet — try again next frame
    }
    const std::string path = bench_capture_request_;
    if (st != ReadbackStatus::Ready) {
        std::println(stderr, "viewer: bench screenshot GPU readback failed");
        benchFinishCapture();
        return;
    }
    const auto png = encodePngRgba8(pixels, bench_grab_rt_.width, bench_grab_rt_.height);
    if (!png.empty()) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (out) {
            out.write(reinterpret_cast<const char *>(png.data()),
                      static_cast<std::streamsize>(png.size()));
        }
    }
    benchFinishCapture();
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

float App::Impl::sceneTargetBytesPerScenePx() const {
    // Per-pixel byte cost of the resolution-scaling offscreen targets — the
    // GPU-format-dependent input to DynamicRenderScale's memory-budget clamp.
    // The targets that grow with resolution are scene color + scene depth (full
    // res) and, when AO is on, the two/three AO targets at ao_resolution_scale²
    // of the scene area. Everything else (IBL cubemaps, the 1x1 AO dummy, the
    // swapchain) is fixed-size and excluded. The per-pixel cost mirrors the
    // format choices ensureSceneTarget makes below, so the estimate tracks
    // HDR / AO toggles automatically.
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
        // buffers, plus the raw target when denoise is on (three total), each at
        // ao_scale² of the scene area.
        const float ao_buffers = quality.enable_ao_denoise ? 3.f : 2.f;
        bytes_per_scene_px +=
            ao_buffers * bytes_per_pixel(pickAoColorFormat()) * ao_scale * ao_scale;
    }
    return bytes_per_scene_px;
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
    const bool exporting_frame = exporter_.renderActive();
    RenderQualitySettings saved_quality;
    if (exporting_frame) {
        saved_quality = quality;
        quality = exporter_.quality();
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
    // Adaptive render scale + all its caps (max texture size, memory budget) are
    // owned by DynamicRenderScale now. Feed it a camera snapshot (a held slider
    // counts as interaction just like a camera move — ImGui::IsAnyItemActive()),
    // per-frame timing, and the GPU-format-derived per-pixel byte cost.
    const CameraSnapshot cam_snapshot{
        .target = camera.target,
        .yaw = camera.yaw,
        .pitch = camera.pitch,
        .distance = camera.distance,
        .fov = camera.fov_deg,
        .proj = camera.projection,
    };
    const DynamicRenderScale::Inputs dyn_inputs{
        .frame_ms = frame_interval_ms,
        .now_seconds = stm_sec(stm_now()),
        .ui_active = ImGui::IsAnyItemActive(),
        .fb_w = fb_width,
        .fb_h = fb_height,
        .limits = {static_cast<std::uint32_t>(sg_query_limits().max_image_size_2d)},
        .bytes_per_scene_px = sceneTargetBytesPerScenePx(),
    };
    const float render_scale = dyn_scale_.update(quality, cam_snapshot, dyn_inputs);
    const auto scale_dim = [render_scale](uint32_t d) -> uint32_t {
        const auto s = static_cast<uint32_t>(static_cast<float>(d) * render_scale + 0.5f);
        return s < 1u ? 1u : s;
    };
    // During an export the offscreen targets are driven straight from the export
    // resolution (the dynamic-scale math above is moot — export_quality disables
    // it — and its result is overridden here). The scene/composite aspect ratio
    // follows scene_rt's dimensions, so this also gives the exported frame the
    // requested output aspect, independent of the window.
    const uint32_t want_w = exporting_frame ? exporter_.internalWidth() : scale_dim(fb_width);
    const uint32_t want_h = exporting_frame ? exporter_.internalHeight() : scale_dim(fb_height);
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
        const bool scene_changing =
            dyn_scale_.interacting() || jobsRunning() || ibl_baker_.dirty(ibl_settings);
        if (scene_changing) {
            last_scene_change = stm_now();
        }
        constexpr double kSceneStableSeconds = 1.0;
        const bool converging =
            last_scene_change != 0 &&
            stm_sec(stm_diff(stm_now(), last_scene_change)) < kSceneStableSeconds;
        // Entering or leaving the overdraw view swaps the scene color between a
        // shaded frame and an accumulated count, so the cached color is invalid
        // across that transition -- force a re-render for the switch frame.
        const bool overdraw_toggled =
            (quality.debug_view == DebugView::Overdraw) != last_rendered_overdraw;
        render_scene = scene_changing || converging || scene_target_resized || overdraw_toggled;
    }

    // Open the GPU-timeline frame. stamp() after each sg_end_pass() below records
    // that pass's GPU cost; endFrame() closes it before sg_commit(). On D3D11 this
    // is the only timing that sees past sokol_app's uninstrumented Present().
    gpu_pass_timer_.beginFrame();

    if (render_scene) {
        // Pass 1 — scene into offscreen color + depth. Depth-clear convention
        // is backend-conditional (see useReversedZ): 0.0 paired with
        // GREATER_EQUAL on `[0,1]` clip-depth backends, 1.0 paired with
        // LESS_EQUAL on GLES3.
        sg_pass scene_pass{};
        scene_pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        // The overdraw debug view accumulates a fragment count additively, so
        // it needs a zero clear; every other view uses the neutral background
        // gray (0x202830).
        scene_pass.action.colors[0].clear_value = (quality.debug_view == DebugView::Overdraw)
                                                      ? sg_color{0.0f, 0.0f, 0.0f, 0.0f}
                                                      : sg_color{0.125f, 0.157f, 0.188f, 1.0f};
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
            const bool cut_ready =
                cfg.boolean_cut && cut_uploaded &&
                build_controller_.cutBuiltStartDeg() == cfg.angle_cut_start_deg &&
                build_controller_.cutBuiltEndDeg() == cfg.angle_cut_end_deg;
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
            flags.material_prefilter = quality.enable_material_prefilter;
            flags.material_prefilter_scale = quality.material_prefilter_scale;
            flags.lod_hull_enable = quality.lod_hull_enable;
            flags.lod_hull_force = quality.lod_hull_force;
            flags.lod_hull_screen_px = quality.lod_hull_screen_px;
            flags.lod_hull_band_px = quality.lod_hull_band_px;
            // Overdraw debug view: draw every group through the additive
            // no-depth pipeline so the color target accumulates a per-pixel
            // fragment count for the composite's heatmap.
            flags.overdraw = quality.debug_view == DebugView::Overdraw;
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
        gpu_pass_timer_.stamp("scene");

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
            gpu_pass_timer_.stamp("ao");

            if (quality.enable_ao_denoise) {
                sg_pass denoise_pass_desc{};
                denoise_pass_desc.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
                denoise_pass_desc.attachments = ao_write.passAttachments();
                denoise_pass_desc.label = "ao_denoise_pass";
                sg_begin_pass(&denoise_pass_desc);
                ao_denoise_pass.draw(ao_rt_raw, scene_rt, camera, ao_write.width, ao_write.height);
                sg_end_pass();
                gpu_pass_timer_.stamp("denoise");
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
        // Remember whether the cached color now holds overdraw counts, so the
        // next frame's pause-when-static gate re-renders on a view transition.
        last_rendered_overdraw = quality.debug_view == DebugView::Overdraw;
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

    // Render the composite + ImGui into the persistent present-cache target
    // rather than straight to the swapchain, then blit the cache into the
    // swapchain. The cache lets the throttle re-present the last full frame with
    // just the blit (presentCachedFrame) — cheaper than re-running composite +
    // ImGui, and it means every D3D11 Present() (sokol issues one unconditionally
    // after every frame callback) scans out a valid frame instead of a stale
    // flip-model back buffer. present_cache_ mirrors the swapchain's color/depth
    // format + (single-sample) count so the composite / ImGui pipelines match.
    ensurePresentCache();
    sg_pass cache_pass{};
    cache_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    cache_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
    cache_pass.attachments = present_cache_.passAttachments();
    cache_pass.label = "present_cache_pass";
    sg_begin_pass(&cache_pass);
    composite.draw(scene_rt, composite_ao_target, ao_pass, quality, last_scene_consumed_ao,
                   camera.near_plane, camera.far_plane, scene_renderer.iblPrefilterView(),
                   scene_renderer.iblCubeSampler(), inv_view_proj, camera.eye());
    ImGui_ImplSokol_Render();
    sg_end_pass();
    // Present-cache pass GPU cost: the fullscreen composite (tonemap + FXAA at
    // full window res) plus the ImGui draw on top.
    gpu_pass_timer_.stamp("composite");

    // Blit the freshly-rendered cache into the swapchain.
    blitPresentCacheToSwapchain();

    // Export capture: composite the same converged frame into the full-res
    // offscreen LDR target (no ImGui). PngExporter::postRender reads it back next.
    if (exporting_frame && exporter_.capture() && exporter_.outTarget().color.id != SG_INVALID_ID) {
        sg_pass export_pass{};
        export_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
        export_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
        export_pass.attachments = exporter_.outTarget().passAttachments();
        export_pass.label = "export_pass";
        sg_begin_pass(&export_pass);
        composite.draw(scene_rt, composite_ao_target, ao_pass, quality, last_scene_consumed_ao,
                       camera.near_plane, camera.far_plane, scene_renderer.iblPrefilterView(),
                       scene_renderer.iblCubeSampler(), inv_view_proj, camera.eye());
        sg_end_pass();
        gpu_pass_timer_.stamp("export");
    }

    // Bench capture: composite the current frame into a window-res LDR target at
    // the *live* quality (not the supersampled export path) so the bench grabs
    // "the current frame". Read back in benchCaptureReadback() after render().
    // Not stamped — this runs on a non-measured frame, so it can't skew timings.
    if (bench_ && !bench_capture_request_.empty() && !bench_capture_issued_) {
        sg_environment benv = sglue_environment();
        sg_pixel_format cfmt = benv.defaults.color_format;
        if (cfmt == SG_PIXELFORMAT_NONE) {
            cfmt = SG_PIXELFORMAT_RGBA8;
        }
        sg_pixel_format dfmt = benv.defaults.depth_format;
        if (dfmt == SG_PIXELFORMAT_NONE) {
            dfmt = SG_PIXELFORMAT_DEPTH;
        }
        if (!bench_grab_rt_.matches(fb_width, fb_height, cfmt, dfmt)) {
            bench_grab_rt_.create(fb_width, fb_height, cfmt, dfmt, /*for_readback=*/true);
        }
        sg_pass grab_pass{};
        grab_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
        grab_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
        grab_pass.attachments = bench_grab_rt_.passAttachments();
        grab_pass.label = "bench_grab_pass";
        sg_begin_pass(&grab_pass);
        composite.draw(scene_rt, composite_ao_target, ao_pass, quality, last_scene_consumed_ao,
                       camera.near_plane, camera.far_plane, scene_renderer.iblPrefilterView(),
                       scene_renderer.iblCubeSampler(), inv_view_proj, camera.eye());
        sg_end_pass();
        bench_capture_issued_ = true;
    }

    gpu_pass_timer_.endFrame();
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

void App::Impl::ensurePresentCache() {
    const sg_environment env = sglue_environment();
    sg_pixel_format cfmt = env.defaults.color_format;
    if (cfmt == SG_PIXELFORMAT_NONE) {
        cfmt = SG_PIXELFORMAT_RGBA8;
    }
    sg_pixel_format dfmt = env.defaults.depth_format;
    if (dfmt == SG_PIXELFORMAT_NONE) {
        dfmt = SG_PIXELFORMAT_DEPTH;
    }
    // Match the swapchain's color+depth format (and single sample count) so the
    // composite / ImGui pipelines — created against the swapchain defaults —
    // validate against this offscreen pass.
    if (!present_cache_.matches(fb_width, fb_height, cfmt, dfmt)) {
        present_cache_.create(fb_width, fb_height, cfmt, dfmt, /*for_readback=*/false);
    }
}

void App::Impl::blitPresentCacheToSwapchain() {
    sg_pass swap_pass{};
    swap_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    swap_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
    swap_pass.swapchain = sglue_swapchain();
    swap_pass.label = "swapchain_blit_pass";
    sg_begin_pass(&swap_pass);
    blit_pass_.draw(present_cache_.color_texture_view);
    sg_end_pass();
}

bool App::Impl::presentCachedFrame() {
    // Nothing cached yet (before the first full frame) — let the caller render
    // normally so the cache gets populated.
    if (present_cache_.color.id == SG_INVALID_ID) {
        return false;
    }
    // If the window resized since the cached frame, its dimensions no longer
    // match the swapchain; blitting would stretch it. A resize bumps the
    // activity clock (full rate), so the very next frame renders fresh anyway —
    // fall through to a real render this frame.
    if (present_cache_.width != static_cast<uint32_t>(sapp_width()) ||
        present_cache_.height != static_cast<uint32_t>(sapp_height())) {
        return false;
    }
    blitPresentCacheToSwapchain();
    sg_commit();
    return true;
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
        build_controller_.clearError();
        return;
    }

    if (decision.kind == Confirm) {
        enqueueProjectDropModal(std::move(decision), [this, path = path]() {
            if (project_ == nullptr) {
                return;
            }
            project_->addPath(path);
            build_controller_.clearError();
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
        build_controller_.clearError();
        return;
    }

    if (decision.kind == Confirm) {
        enqueueProjectDropModal(std::move(decision), [this, filename = filename, bytes = bytes]() {
            if (project_ == nullptr) {
                return;
            }
            project_->addBytes(filename, std::span<const std::byte>{bytes.data(), bytes.size()});
            build_controller_.clearError();
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
    const bool exporting = exporter_.active();
    const bool keep_loop_awake = exporting || startup_screenshot_pending;

    // Procedural IBL bake — debounce, dirty-track, bake, install. Runs at the
    // top of onFrame (before the swapchain pass that draws the scene) because a
    // bake pass can't be issued from inside that pass; same-frame ordering is
    // fine — sokol guarantees images written by an earlier pass are sampleable
    // in a later pass within the same frame. The GPU bake + install + toast is
    // wired into ibl_baker_ in onInit().
    ibl_baker_.poll(ibl_settings, std::chrono::steady_clock::now());

    // Drive the CPU build orchestration: the in-flight tessellation job, the
    // project walk / session, and any queued Boolean-cut rebuild. Completed
    // scenes route back to the App's GPU/scene state via the callbacks wired in
    // onInit(). The project is long-lived (drops/picks accumulate into the bag),
    // so the controller keeps polling it every frame.
    build_controller_.poll(
        project_.get(),
        BuildController::AngleCut{cfg.boolean_cut, cfg.angle_cut_start_deg, cfg.angle_cut_end_deg},
        cut_uploaded);

    // Benchmark mode takes over the frame entirely: it drives the camera/state
    // sequence and renders at full rate, bypassing the idle/UI/input path below.
    // The async polls above still run, so builds/bakes (incl. the cut rebuild the
    // sequence triggers) progress normally.
    if (bench_) {
        benchFrame();
        return;
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
        if (!jobsRunning()) {
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
        const bool full_rate = input_recent || jobsRunning() || ibl_baker_.dirty(ibl_settings) ||
                               cfg.auto_orbit || drag_hover || pending_gesture ||
                               notifications.hasActiveToasts();
        if (!full_rate && last_time != 0 &&
            stm_sec(stm_diff(stm_now(), last_time)) < kIdleFrameInterval) {
            // Re-present the last full frame with a cheap blit instead of an
            // early return. sokol issues a D3D11 Present() unconditionally after
            // this callback returns, and on the flip-model swapchain a frame we
            // didn't draw scans out a stale back buffer — which reads as the perf
            // plot jumping backward. The blit keeps every present valid and still
            // skips the expensive composite + ImGui rebuild. If no cache exists
            // yet, fall through and render a full frame to populate it.
            if (presentCachedFrame()) {
                return;
            }
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
            // Blit the cached frame on the dropped vsync so it still presents a
            // valid frame on D3D11 rather than a stale flip buffer (see the pause
            // gate above); fall through to a real render if there's no cache yet.
            if (presentCachedFrame()) {
                return;
            }
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
    const GpuPassTimings &gpu_times = gpu_pass_timer_.results();
    perf_history.push(delta_seconds, frame_interval_ms, encode_ms, present_ms, gpu_wait_ms,
                      scene_submit_ms, gpu_times.valid ? gpu_times.total_ms : 0.0, fps);

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

    ui::ViewerUiContext ui_ctx{
        .cfg = cfg,
        .quality = quality,
        .export_settings = export_settings,
        .project = project_.get(),
        .build_session = build_controller_.session(),
        .build_job = build_controller_.job(),
        // Stats reflect the renderer drawn last frame (base or cut). active_
        // renderer is updated in render(); a one-frame lag here is harmless.
        .scene_renderer = (active_renderer != nullptr) ? *active_renderer : scene_renderer,
        .camera = camera,
        .notifications = &notifications,
        .platform_window_state = platform_window_state,
        .root_config_key = build_controller_.rootConfigKey(),
        .root_geometry_key = build_controller_.rootGeometryKey(),
        .build_error = build_controller_.error(),
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
        .gpu_pass_times = &gpu_pass_timer_.results(),
        .has_scene = static_cast<bool>(scene),
        .scene_uploaded = scene_uploaded,
        .build_in_progress = build_controller_.inProgress(),
        .export_in_progress = exporting,
        .ibl_installed = ibl_baker_.installed(),
        .hdr_supported = hdrSupported(),
        .ibl_settings = &ibl_settings,
    };

    ui::UiActions ui_actions;
    ui_actions.sync_browser_url = [this]() { syncBrowserUrl(); };
    ui_actions.open_url = [this](const std::string &url) { platform_->openUrl(url); };
    ui_actions.rebake_ibl = [this]() { ibl_baker_.requestRebake(); };
    ui_actions.request_scene_rebuild = [this]() { build_controller_.requestCutRebuild(); };
    ui_actions.export_png = [this]() {
        exporter_.request(export_settings, quality, scene != nullptr && scene_uploaded);
    };
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
        project_ = platform::makeEmptyBag();
        project_->setLogSink(&notifications);
        // Drops the pristine cache, root keys, session keys, the pending-cut
        // flag, and the persistent error in one shot.
        build_controller_.reset();
        active_modals.clear();
        scene_uploaded = false;
        cut_uploaded = false;
        camera_framed = false;
        scene_radius = 0.f;
        notifications.info("Project closed");
    };
    ui_actions.rescan_project = [this]() {
        if (project_) {
            project_->rescan();
            build_controller_.clearError();
            notifications.info("Project rescan requested");
        }
    };
    ui_actions.select_config_key = [this](std::string key) {
        build_controller_.setRootConfigKey(std::move(key));
    };
    ui_actions.select_geometry_key = [this](std::string key) {
        build_controller_.setRootGeometryKey(std::move(key));
    };

    ui::renderViewerUi(ui_state, ui_ctx, ui_actions);
    renderActiveModal();
    notifications.render();

    // Headless screenshot: once the scene is loaded, uploaded, lit and framed,
    // fire the one-shot export (which quits when the file is written).
    //
    // When a Boolean cut is enabled the base (uncut) scene lands first and the
    // cut bake follows asynchronously (see BuildController: base build is always
    // wedge=nullopt, then a pending cut rebuild re-tessellates the wedge). In
    // that interim the app draws the uncut base with the live *shader* discard —
    // which exposes raw interior faces and is not the intended capped cut view.
    // Gate the export on the cut bake being resident and matching the committed
    // angle so headless renders capture the finished Boolean-cut scene, not the
    // interim preview. (No-op when boolean_cut is off — nothing to wait for.)
    const bool cut_settled =
        !cfg.boolean_cut || (cut_uploaded && !build_controller_.pendingCutRebuild() &&
                             build_controller_.cutBuiltStartDeg() == cfg.angle_cut_start_deg &&
                             build_controller_.cutBuiltEndDeg() == cfg.angle_cut_end_deg);

    // A headless screenshot has no UI to surface a stuck build: without this
    // check, a missing/unresolvable config or geometry key (or a project/build
    // failure) would leave startup_screenshot_pending true forever and the
    // process would hang instead of ever producing the PNG. Fail loudly and
    // exit non-zero the moment any of those terminal error states appears.
    if (startup_screenshot_pending) {
        std::string fatal;
        if (project_ && project_->status() == ProjectFsStatus::Error) {
            fatal = "failed to load input project/geometry file";
        } else if (build_controller_.session().phase() == BuildPhase::Error) {
            fatal = "failed to resolve or parse the config/geometry";
        } else if (build_controller_.session().phase() == BuildPhase::WaitingForUser) {
            std::string missing_keys;
            for (const auto &key : build_controller_.session().missing()) {
                if (!missing_keys.empty()) {
                    missing_keys += ", ";
                }
                missing_keys += key;
            }
            fatal = std::format("missing required config/geometry: {}", missing_keys);
        } else if (!build_controller_.error().empty()) {
            fatal = build_controller_.error();
        }
        if (!fatal.empty()) {
            std::println(stderr, "viewer: --screenshot failed: {}", fatal);
            std::exit(1);
        }
    }

    if (startup_screenshot_pending && !exporter_.active() && scene && scene_uploaded &&
        ibl_baker_.installed() && camera_framed && !build_controller_.inProgress() && cut_settled) {
        startup_screenshot_pending = false;
        exporter_.request(startup_screenshot_settings, quality, scene != nullptr && scene_uploaded,
                          startup_screenshot_path, /*quit_when_done=*/true);
    }

    // Set up any export-frame state (target allocation, dims/quality override
    // flags) before render() reads it; advance the export state machine after.
    exporter_.preRender();

    // simgui_render (called from inside render() → ImGui_ImplSokol_Render)
    // internally calls ImGui::Render itself before issuing draws.
    render();

    exporter_.postRender();

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
    blit_pass_.release();
    present_cache_.destroy();
    ao_denoise_pass.release();
    ao_pass.release();
    ao_hist[0].destroy();
    ao_hist[1].destroy();
    ao_rt_raw.destroy();
    exporter_.destroyTargets();
    bench_grab_rt_.destroy();
    bench_readback_.reset();
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

void App::requestBench(std::string json_out_path, std::string scene_label, float render_scale) {
    impl_->bench_pending_ = true;
    impl_->bench_json_path_ = std::move(json_out_path);
    impl_->bench_scene_label_ = std::move(scene_label);
    impl_->bench_render_scale_ = render_scale;
    // vsync feeds swap_interval, which run() reads before onInit — so it must be
    // forced here, before run(). The rest of the determinism knobs are asserted
    // in benchInit() (after persisted state loads and could clobber them).
    impl_->cfg.vsync = false;
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
    impl_->build_controller_.setRootKeys({}, {});
    impl_->active_modals.clear();
}

void App::setRootKeys(std::string config_key, std::string geometry_key) {
    impl_->build_controller_.setRootKeys(std::move(config_key), std::move(geometry_key));
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

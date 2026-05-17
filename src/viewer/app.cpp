#include <nodehammer/viewer/app.hpp>

#include "ao_denoise_pass.hpp"
#include "ao_pass.hpp"
#include "ao_render_target.hpp"
#include "composite_pass.hpp"
#include "ibl.hpp"
#include "imgui_backend.hpp"
#include "scene_build_job.hpp"
#include "scene_render_target.hpp"
#include "scene_renderer.hpp"
#include "ui/icon_font.hpp"
#include "ui/notifications.hpp"
#include "ui/viewer_ui.hpp"

#include <nodehammer/viewer/backend_caps.hpp>
#include <nodehammer/viewer/platform.hpp>
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
#include <filesystem>
#include <format>
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

} // namespace

constexpr const char *kViewerConfigStateKey = "viewer-state.toml";
constexpr const char *kImGuiStateKey = "imgui.ini";
constexpr double kPersistenceSaveIntervalSeconds = 1.0;
constexpr std::chrono::milliseconds kIblRebakeDebounce{300};

struct App::Impl {
    Config cfg;
    bool launch_had_initial_camera{false};
    bool quit{false};

    std::shared_ptr<const RenderScene> scene;
    std::unique_ptr<ProjectFs> project_;
    /// Platform impl: native vs web. Constructed by App's ctor body
    /// after this Impl is in place, so the impl can hold a back-pointer
    /// to the live App. State that would otherwise live in file-static
    /// globals (picker latches, window hooks) lives as platform members.
    std::unique_ptr<platform::Platform> platform_;
    platform::WindowCustomizationRequest window_customization;
    platform::PlatformWindowState platform_window_state;
    std::vector<platform::PlatformGestureEvent> platform_gesture_events;
    SceneRenderer scene_renderer;
    SceneRenderTarget scene_rt;
    // Two AO targets: GTAO writes raw into ao_rt_raw; the bilateral denoise
    // pass reads from raw + scene depth and writes the denoised result into
    // ao_rt_history. The *next* frame's scene pass samples ao_rt_history
    // for the bent-normal IBL / multi-bounce / specular-occlusion path,
    // and the current frame's composite samples it for the Lambert AO
    // fallback. `ao_history_valid` gates the scene-side sample on the
    // first frame (before any denoise has run).
    AoRenderTarget ao_rt_raw;
    AoRenderTarget ao_rt_history;
    bool ao_history_valid{false};
    /// True when the previous frame ran with denoise disabled and so the
    /// "history" data actually lives in `ao_rt_raw` rather than
    /// `ao_rt_history`. The scene shader reads last frame's AO, so this
    /// bit decides which texture *this* frame's scene pass binds. Updated
    /// at the end of every frame after the AO+denoise (or AO-only) passes
    /// have run.
    bool ao_history_was_raw{false};
    AoPass ao_pass;
    AoDenoisePass ao_denoise_pass;
    CompositePass composite;
    RenderQualitySettings quality;
    Camera camera;

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

    // Sokol-time stamp of the last user-input-class event. Drives the
    // idle-throttle decision: a recent input keeps us at full vsync
    // rate even when the OS reports the window as backgrounded, so
    // scroll-wheel control and the drag overlay stay reactive. 0 means
    // "no input observed yet" — treated as idle.
    uint64_t last_activity_time{0};

    uint64_t frame_count{0};
    uint64_t fps_window_start{0};
    float fps{0.f};
    double frame_interval_ms{0.0};
    double render_submit_ms{0.0};
    double scene_submit_ms{0.0};

    ScrollInputMode scroll_input_mode{ScrollInputMode::Wheel};
    float pending_scroll_x{0.f};
    float pending_scroll_y{0.f};
    uint32_t pending_scroll_modifiers{0};
    uint64_t last_scroll_time{0};
    int smooth_scroll_score{0};
    int wheel_scroll_score{0};

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

    // The renderer initialises with 1×1 placeholder IBL textures so the
    // first frame can draw before the GPU bake runs; onFrame swaps in the
    // real result on the first tick.
    scene_renderer.initialize();
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
    classifyScroll(ev->scroll_x, ev->scroll_y);

    const ImGuiIO &io = ImGui::GetIO();
    if (imgui_handled || io.WantCaptureMouse) {
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
            } else {

                constexpr float kTrackpadOrbitSensitivity = platform::kIsWeb ? 0.08f : 0.03f;
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
    const int samples = 1;
    if (!scene_rt.matches(width, height, color_fmt, depth_fmt, samples)) {
        scene_rt.create(width, height, color_fmt, depth_fmt, samples);
    }
    scene_renderer.setTargetColorFormat(color_fmt);

    // AO targets share dimensions with the scene RT but are allocated only
    // when AO is enabled. Two targets coexist: `ao_rt_raw` (GTAO writes
    // here) and `ao_rt_history` (denoise writes here; sampled by next
    // frame's scene shader and by this frame's composite-Lambert path).
    // Cached across enable/disable toggles so repeated toggling doesn't
    // churn the image pool — only a resize destroys them. A resize also
    // invalidates the history target's contents (different dimensions =
    // different per-pixel samples), so flip the validity bit.
    const sg_pixel_format ao_fmt = pickAoColorFormat();
    if (quality.enable_ao) {
        if (!ao_rt_raw.matches(width, height, ao_fmt)) {
            ao_rt_raw.create(width, height, ao_fmt);
            ao_history_valid = false; // raw + history were paired by dimension
        }
        if (!ao_rt_history.matches(width, height, ao_fmt)) {
            ao_rt_history.create(width, height, ao_fmt);
            ao_history_valid = false;
        }
        ao_pass.setTargetColorFormat(ao_fmt);
        ao_denoise_pass.setTargetColorFormat(ao_fmt);
    } else {
        if (ao_rt_raw.color.id != SG_INVALID_ID && !ao_rt_raw.matches(width, height, ao_fmt)) {
            ao_rt_raw.destroy();
        }
        if (ao_rt_history.color.id != SG_INVALID_ID &&
            !ao_rt_history.matches(width, height, ao_fmt)) {
            ao_rt_history.destroy();
        }
        // AO is off — next enable will start fresh and rebuild history.
        ao_history_valid = false;
    }
}

void App::Impl::render() {
    const uint64_t render_submit_start = stm_now();
    scene_submit_ms = 0.0;
    bool scene_consumed_ao_this_frame = false;

    // Drive the chunked GPU upload BEFORE sg_begin_pass so any new sokol
    // buffer creation isn't tangled up with the active scene pass.
    if (scene && !scene_uploaded) {
        if (!scene_renderer.uploadInProgress()) {
            scene_renderer.beginUpload(scene);
        }
        if (scene_renderer.advanceUpload()) {
            scene_uploaded = true;
        }
    }

    // Lazily allocate (or reallocate on resize / DPI change / future
    // MSAA toggle) the offscreen scene target. Doing this here, just
    // before the pass begins, avoids reallocating from inside an event
    // callback while a frame may still be in flight.
    ensureSceneTarget(fb_width, fb_height);

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
    sg_begin_pass(&scene_pass);

    if (scene && scene_uploaded) {
        if (!camera_framed) {
            glm::vec3 bmin{0.f}, bmax{0.f};
            if (scene_renderer.worldBounds(bmin, bmax)) {
                scene_radius = camera.frameBounds(bmin, bmax);
                applyInitialCamera();
                camera_framed = true;
            }
        }
        SceneRenderer::RenderFlags flags;
        flags.cull = cfg.cull;
        flags.angle_cut = cfg.angle_cut;
        flags.shader_angle_cut = cfg.shader_angle_cut;
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
        // Pick the right "history" target based on what last frame produced:
        // either the denoised target (normal case) or the raw target (last
        // frame had denoise toggled off). Falls back to a null view when
        // history isn't usable; scene shader's enable bit gates the sample.
        const AoRenderTarget &history_src = ao_history_was_raw ? ao_rt_raw : ao_rt_history;
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
        scene_renderer.render(camera, scene_rt.width, scene_rt.height, flags);
        scene_submit_ms = stm_sec(stm_diff(stm_now(), scene_submit_start)) * 1000.0;
    }

    sg_end_pass();

    // Pass 2 (optional) — GTAO into the raw AO target, then a bilateral
    // denoise from raw into the history target. Skipped entirely when
    // disabled or while a depth-debug view is active (the composite short-
    // circuits AO in those modes anyway, but skipping the passes saves the
    // work). The denoise output (history) is read by *next* frame's scene
    // shader (PBR IBL) and by this frame's composite Lambert-AO path.
    //
    // When `enable_ao_denoise` is off, the denoise pass is skipped and the
    // raw target stands in as "history" — scene/composite both rebind to
    // `ao_rt_raw` via the gating below so they see the un-denoised data
    // (useful as a diagnostic A/B against the denoised path).
    const bool ao_active = quality.enable_ao && quality.debug_view == DebugView::Off &&
                           ao_rt_raw.color.id != SG_INVALID_ID &&
                           ao_rt_history.color.id != SG_INVALID_ID && scene_uploaded;
    bool ao_history_target_is_raw = false;
    if (ao_active) {
        sg_pass ao_pass_desc{};
        ao_pass_desc.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
        ao_pass_desc.attachments = ao_rt_raw.passAttachments();
        ao_pass_desc.label = "ao_pass";
        sg_begin_pass(&ao_pass_desc);
        ao_pass.draw(scene_rt, camera, ao_rt_raw.width, ao_rt_raw.height, quality);
        sg_end_pass();

        if (quality.enable_ao_denoise) {
            sg_pass denoise_pass_desc{};
            denoise_pass_desc.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            denoise_pass_desc.attachments = ao_rt_history.passAttachments();
            denoise_pass_desc.label = "ao_denoise_pass";
            sg_begin_pass(&denoise_pass_desc);
            ao_denoise_pass.draw(ao_rt_raw, scene_rt, camera, ao_rt_history.width,
                                 ao_rt_history.height);
            sg_end_pass();
        } else {
            // Denoise toggle is off — leave ao_rt_history un-updated and
            // let downstream consumers rebind to the raw target. The
            // history-validity bit is still flipped on because the raw
            // target's contents are valid (just noisier than denoised).
            ao_history_target_is_raw = true;
        }

        // AO output is populated and safe for next frame's scene sample
        // (either the just-denoised history target or, when denoise is
        // off, the raw target — see the rebind below).
        ao_history_valid = true;
        ao_history_was_raw = ao_history_target_is_raw;
    }

    // Pass 3 — composite the offscreen target into the swapchain, then
    // ImGui on top. The composite covers the entire viewport so we
    // don't need to clear color or depth first.
    sg_pass swap_pass{};
    swap_pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    swap_pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
    swap_pass.swapchain = sglue_swapchain();
    swap_pass.label = "swapchain_pass";
    sg_begin_pass(&swap_pass);

    {
        // Match the conventions the scene shader uses (see scene_renderer.cpp):
        // GL/GLES needs [-1,1] z; everything else [0,1]. Reversed-Z is gated
        // by useReversedZ(). Compute inv(view_proj) so the composite FS can
        // turn a screen-space pixel into a world-space view ray for the
        // background dome.
        const sg_backend backend = sg_query_backend();
        const bool homogeneous_depth =
            (backend == SG_BACKEND_GLCORE) || (backend == SG_BACKEND_GLES3);
        const float aspect = (scene_rt.height > 0) ? static_cast<float>(scene_rt.width) /
                                                         static_cast<float>(scene_rt.height)
                                                   : 1.0f;
        const glm::mat4 view_proj =
            camera.proj(aspect, homogeneous_depth, useReversedZ()) * camera.view();
        const glm::mat4 inv_view_proj = glm::inverse(view_proj);
        // Composite samples the *denoised* target (ao_rt_history) for its
        // Lambert AO multiply — that's the same data the next frame's
        // scene-shader PBR path will sample, kept consistent so toggling
        // PBR on/off doesn't introduce a frame of mismatched AO. The
        // `ao_already_applied_in_scene` flag mirrors the actual scene
        // decision (captured above before the AO+denoise passes ran) so
        // the first frame after enable — where history flips valid mid-
        // frame — still gets a composite AO multiply rather than no AO at
        // all.
        //
        // When the history target hasn't been written yet (very first frame
        // after enable, or before the scene has finished uploading), pass
        // an empty render target — composite's internal `ao_on` check
        // gates on a valid color id and falls back to its 1×1 white dummy
        // so we never sample undefined data. When denoise is off this
        // frame, the raw target is what the composite should sample.
        const AoRenderTarget *composite_ao_src = nullptr;
        if (ao_history_valid) {
            composite_ao_src = ao_history_target_is_raw ? &ao_rt_raw : &ao_rt_history;
        }
        const AoRenderTarget composite_ao_empty{};
        const AoRenderTarget &composite_ao_target =
            composite_ao_src ? *composite_ao_src : composite_ao_empty;
        composite.draw(scene_rt, composite_ao_target, ao_pass, quality,
                       scene_consumed_ao_this_frame, camera.near_plane, camera.far_plane,
                       scene_renderer.iblPrefilterView(), scene_renderer.iblCubeSampler(),
                       inv_view_proj, camera.eye());
    }
    ImGui_ImplSokol_Render();

    sg_end_pass();
    sg_commit();
    render_submit_ms = stm_sec(stm_diff(stm_now(), render_submit_start)) * 1000.0;
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
            "cull,pauseWhenUnfocused,autoOrbit,orbitSpeed,angleCut,shaderAngleCut,cutStart,"
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
        scene_renderer.installIbl(bakeIblGpu(ibl_settings));
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
            const auto total = build_job.tessellationTotal();
            const auto processed = build_job.tessellationProcessed();
            const float frac =
                total > 0 ? static_cast<float>(processed) / static_cast<float>(total) : 0.0f;
            std::string label;
            if (total > 0) {
                label = std::format("Tessellating ({}/{} nodes)", processed, total);
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
            scene = std::move(built.scene);
            scene_uploaded = false;
            camera_framed = false;
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
    if (idle) {
        const auto session_phase = build_session.phase();
        const bool jobs_running =
            !ibl_installed || build_in_progress || (scene && !scene_uploaded) ||
            session_phase == BuildPhase::Walking || session_phase == BuildPhase::ResolvedReady;
        if (!jobs_running) {
            return;
        }
        constexpr double kIdleFrameInterval = 1.0 / 30.0; // 30 Hz
        if (stm_sec(stm_diff(stm_now(), last_time)) < kIdleFrameInterval) {
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

    // simgui_new_frame internally calls ImGui::NewFrame after configuring
    // io display size + delta time. Don't double-call NewFrame.
    ImGui_ImplSokol_NewFrame(static_cast<int>(fb_width), static_cast<int>(fb_height), delta_seconds,
                             sapp_dpi_scale());

    platform_->beginFrameWindowSync();
    platform_window_state = platform_->windowState();
    platform_gesture_events = platform_->takeGestureEvents();

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
    if (project_) {
        project_->poll();
        build_session.poll(project_.get());

        if (!build_in_progress && build_session.phase() == BuildPhase::ResolvedReady) {
            if (auto inputs = build_session.takeInputs()) {
                build_start_time = std::chrono::steady_clock::now();
                build_job.start(std::move(inputs->config.config), std::move(inputs->import.scene),
                                std::move(inputs->config_key), std::move(inputs->geometry_key));
                build_in_progress = true;
                build_progress_handle = notifications.startProgress("Tessellating...");
            }
        }
    }

    ui::ViewerUiContext ui_ctx{
        .cfg = cfg,
        .quality = quality,
        .project = project_.get(),
        .build_session = build_session,
        .build_job = build_job,
        .scene_renderer = scene_renderer,
        .camera = camera,
        .notifications = &notifications,
        .platform_window_state = platform_window_state,
        .root_config_key = root_config_key,
        .root_geometry_key = root_geometry_key,
        .build_error = build_error,
        .fb_width = fb_width,
        .fb_height = fb_height,
        .fps = fps,
        .frame_interval_ms = frame_interval_ms,
        .render_submit_ms = render_submit_ms,
        .scene_submit_ms = scene_submit_ms,
        .has_scene = static_cast<bool>(scene),
        .scene_uploaded = scene_uploaded,
        .build_in_progress = build_in_progress,
        .ibl_installed = ibl_installed,
        .hdr_supported = hdrSupported(),
        .ibl_settings = &ibl_settings,
    };

    ui::UiActions ui_actions;
    ui_actions.sync_browser_url = [this]() { syncBrowserUrl(); };
    ui_actions.open_url = [this](const std::string &url) { platform_->openUrl(url); };
    ui_actions.rebake_ibl = [this]() { ibl_rebake_pending = true; };
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
        scene.reset();
        project_ = platform::makeEmptyBag();
        project_->setLogSink(&notifications);
        root_config_key.clear();
        root_geometry_key.clear();
        build_session.setRootKeys({}, {});
        active_modals.clear();
        scene_uploaded = false;
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

    // simgui_render (called from inside render() → ImGui_ImplSokol_Render)
    // internally calls ImGui::Render itself before issuing draws.
    render();

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
    ao_rt_history.destroy();
    ao_rt_raw.destroy();
    scene_rt.destroy();
    scene_renderer.release();
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

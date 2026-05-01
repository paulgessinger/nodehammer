#include <nodehammer/viewer/app.hpp>

#include "ibl.hpp"
#include "ibl_cache.hpp"
#include "imgui_backend.hpp"
#include "scene_build_job.hpp"
#include "scene_renderer.hpp"
#include <nodehammer/viewer/platform.hpp>

#include <nodehammer/ir/render.hpp>
#include <nodehammer/scene_build.hpp>
#include <nodehammer/viewer/bag_project_fs.hpp>
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
#include <iomanip>
#include <memory>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

enum class ScrollInputMode { Wheel, Trackpad };

bool isZoomModifier(uint32_t modifiers) {
    return (modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER)) != 0;
}

float wrapDegrees(float angle) {
    angle = std::fmod(angle, 360.f);
    if (angle < 0.f) {
        angle += 360.f;
    }
    return angle;
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

std::string projectTreeSelectionKey(std::string_view key) {
    auto out = std::filesystem::path{key}.lexically_normal().generic_string();
    if (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return out;
}

} // namespace

struct App::Impl {
    Config cfg;
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
    Camera camera;

    // Procedural IBL bake. Started right after sg_setup so we don't pay
    // the cost on first scene-load. Native runs the bake on a worker
    // thread; web time-slices it on the main thread inside `advance`.
    IblBakeJob ibl_job;
    bool ibl_installed{false};
    std::chrono::steady_clock::time_point ibl_start_time{};

    // On startup we first try to load a previously baked IBL from the
    // platform cache (file on native, IndexedDB on web). If it hits, we
    // skip the bake entirely; on a miss we kick off `ibl_job`. The native
    // path resolves the hit/miss decision synchronously inside `start()`;
    // the web path resolves it asynchronously, which is why this is its
    // own state machine instead of a plain bool.
    IblCacheLoad ibl_cache_load;
    bool ibl_cache_decided{false};
    bool ibl_cache_hit{false};

    // Off-loop scene tessellation. Native runs the build on a worker
    // thread so the UI stays smooth. Web defers the synchronous build by
    // one frame so the previous frame paints a "Tessellating…" message
    // before the page freezes.
    SceneBuildJob build_job;
    bool build_in_progress{false};
    std::chrono::steady_clock::time_point build_start_time{};

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

    explicit Impl(Config c) : cfg(std::move(c)), project_(std::make_unique<BagProjectFs>()) {}

    void onInit();
    void onFrame();
    void onEvent(const sapp_event *ev);
    void onCleanup();

    void classifyScroll(float scroll_x, float scroll_y);
    void handleScrollEvent(const sapp_event *ev, bool imgui_handled);
    void updateCameraInput();
    void applyInitialCamera();
    void render();
    void syncBrowserUrl() const;
    [[nodiscard]] std::string browserUrlStateQuery() const;

    static void initCb(void *user) { static_cast<Impl *>(user)->onInit(); }
    static void frameCb(void *user) { static_cast<Impl *>(user)->onFrame(); }
    static void eventCb(const sapp_event *ev, void *user) {
        static_cast<Impl *>(user)->onEvent(ev);
    }
    static void cleanupCb(void *user) { static_cast<Impl *>(user)->onCleanup(); }
};

void App::Impl::onInit() {
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

    // Kick off the IBL bake (or cache load) immediately. The renderer is
    // initialised with 1×1 placeholder IBL textures so it can render before
    // the bake completes; onFrame swaps in the real result once either
    // the cache load resolves with a hit, or `ibl_job.advance` returns true.
    scene_renderer.initialize();
    ibl_start_time = std::chrono::steady_clock::now();
    ibl_cache_load.start();

    stm_setup();
    last_time = stm_now();

    IMGUI_CHECKVERSION();
    // simgui_setup creates the ImGui context, applies the dark style, and
    // sets ini_filename internally — DO NOT call ImGui::CreateContext or
    // ImGui::StyleColorsDark here, that would double-init and crash on
    // simgui_shutdown.
    ImGui_ImplSokol_Init();

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
        quit = true;
    } else if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        // Platform code pushes the dropped files into the App's existing
        // project (synchronously on native, via per-file fetch callbacks
        // on web). Lives in platform-specific code.
        platform_->dispatchDroppedFiles();
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

void App::Impl::render() {
    const uint64_t render_submit_start = stm_now();
    scene_submit_ms = 0.0;

    // Drive the chunked GPU upload BEFORE sg_begin_pass so any new sokol
    // buffer creation isn't tangled up with the active swapchain pass.
    if (scene && !scene_uploaded) {
        if (!scene_renderer.uploadInProgress()) {
            scene_renderer.beginUpload(scene);
        }
        if (scene_renderer.advanceUpload()) {
            scene_uploaded = true;
        }
    }

    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {0.125f, 0.157f, 0.188f, 1.0f}; // 0x202830
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    // Reversed-Z: clear to 0 (the "farthest" depth in our convention).
    pass.action.depth.clear_value = 0.0f;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);

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
        flags.cull_back = cfg.cull_back;
        flags.angle_cut = cfg.angle_cut;
        flags.shader_angle_cut = cfg.shader_angle_cut;
        flags.angle_cut_start_deg = cfg.angle_cut_start_deg;
        flags.angle_cut_end_deg = cfg.angle_cut_end_deg;
        flags.enable_pbr = cfg.enable_pbr;
        const uint64_t scene_submit_start = stm_now();
        scene_renderer.render(camera, fb_width, fb_height, flags);
        scene_submit_ms = stm_sec(stm_diff(stm_now(), scene_submit_start)) * 1000.0;
    }

    ImGui_ImplSokol_Render();

    sg_end_pass();
    sg_commit();
    render_submit_ms = stm_sec(stm_diff(stm_now(), render_submit_start)) * 1000.0;
}

std::string App::Impl::browserUrlStateQuery() const {
    std::string query;
    appendUrlBool(query, "cullBack", cfg.cull_back, true);
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
    return query;
}

void App::Impl::syncBrowserUrl() const {
    if constexpr (platform::kIsWeb) {
        const std::string state_query = browserUrlStateQuery();
        constexpr const char *kManagedKeys =
            "cullBack,pauseWhenUnfocused,autoOrbit,orbitSpeed,angleCut,shaderAngleCut,cutStart,"
            "cutEnd,"
            "pbr,cameraTargetX,cameraTargetY,cameraTargetZ,cameraDistance,cameraYaw,cameraPitch";
        platform_->commitUrlState(state_query, kManagedKeys);
    }
}

void App::Impl::onFrame() {
    // Drive the procedural IBL bake to completion. On native this is a
    // single-flag poll (the bake runs on a worker thread); on web this
    // spends up to ~8 ms doing pixel work on the main thread. We don't
    // gate this on focus: the bake should finish whether or not the user
    // is looking at the window.
    if (!ibl_installed) {
        if (!ibl_cache_decided) {
            // Wait for the cache load to resolve. On native this is true on
            // the first frame; on web it may take a few frames while the
            // IndexedDB get round-trips through the JS event loop.
            if (ibl_cache_load.poll()) {
                if (auto cached = ibl_cache_load.take()) {
                    scene_renderer.installIbl(*cached);
                    ibl_installed = true;
                    ibl_cache_hit = true;
                    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - ibl_start_time)
                                                .count();
                    std::println("viewer: IBL loaded from cache ({:.1f} ms)", elapsed_ms);
                } else {
                    // Miss — start the real bake now. ibl_start_time stays
                    // anchored at the cache attempt so the reported total
                    // includes both the failed lookup and the bake itself.
                    ibl_job.start();
                }
                ibl_cache_decided = true;
            }
        } else if (!ibl_cache_hit && ibl_job.advance()) {
            auto data = ibl_job.take();
            // Persist before installing so we don't pay the wait twice if
            // the user reloads while the bake is technically "live" on GPU
            // but not yet flushed to disk / IDB.
            saveIblCache(data);
            scene_renderer.installIbl(data);
            ibl_installed = true;
            const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - ibl_start_time)
                                        .count();
            std::println("viewer: IBL bake complete ({:.1f} ms)", elapsed_ms);
        }
    }

    // Drive the off-loop tessellation. On native this is a poll of an
    // atomic flag set by the worker thread; on web it runs the build
    // synchronously on the second poll (the first paints a frame).
    if (build_in_progress && build_job.poll()) {
        auto built = build_job.take();
        for (const auto &d : built.diags.items()) {
            std::println(stderr, "scene_build: {} {}", d.code, d.message);
        }
        if (built.scene) {
            const auto build_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - build_start_time)
                                      .count();
            std::println("viewer: tessellation complete ({:.1f} ms, {} nodes, {} mesh assets, "
                         "{} materials)",
                         build_ms, built.scene->nodes.size(), built.scene->meshAssets.size(),
                         built.scene->materials.size());
            scene = std::move(built.scene);
            scene_uploaded = false;
            camera_framed = false;
            build_error.clear();
        } else {
            build_error = "scene build failed";
            for (const auto &d : built.diags.items()) {
                if (d.severity >= DiagnosticSeverity::Error) {
                    build_error = d.message;
                    break;
                }
            }
        }
        build_in_progress = false;
        // Project is long-lived: we keep it so additional drops/picks
        // accumulate into the existing bag (or so a UrlProjectFs's state
        // survives in case the user wants to inspect what loaded). The
        // build job already has the paths it needs; nothing else to do.
    }

    // When unfocused, throttle to a low rate (~5 Hz) instead of pausing
    // entirely. Full pause was visible as "drag-and-drop into a
    // background viewer feels broken": the drop event fired and the
    // async project/scene-build state advanced, but no frame rendered the
    // result until focus returned. 5 Hz keeps drag-over feedback and
    // load progress visible while still cutting GPU cost ~12x vs. the
    // active 60 Hz path. The flag's URL/persistence name stays
    // `pauseWhenUnfocused` for backwards compatibility.
    if (cfg.pause_when_unfocused && (!window_focused || !window_visible)) {
        constexpr double kIdleFrameInterval = 0.5; // 2 Hz
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

    const auto &chrome = platform_window_state.chrome;
    if (chrome.titlebar_transparent && chrome.traffic_lights_overlap_content) {
        ImGui::SetNextWindowPos({chrome.content_left_inset + 8.f, chrome.content_top_inset + 8.f},
                                ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize({500, 1000}, ImGuiCond_FirstUseEver);
    ImGui::Begin("nodehammer viewer");
    ImGui::Text("Backbuffer: %u x %u", fb_width, fb_height);
    {
        const sg_backend backend = sg_query_backend();
        const char *name = "?";
        switch (backend) {
        case SG_BACKEND_GLCORE:
            name = "GL";
            break;
        case SG_BACKEND_GLES3:
            name = "GLES3 / WebGL2";
            break;
        case SG_BACKEND_D3D11:
            name = "D3D11";
            break;
        case SG_BACKEND_METAL_IOS:
            name = "Metal (iOS)";
            break;
        case SG_BACKEND_METAL_MACOS:
            name = "Metal (macOS)";
            break;
        case SG_BACKEND_METAL_SIMULATOR:
            name = "Metal (sim)";
            break;
        case SG_BACKEND_WGPU:
            name = "WebGPU";
            break;
        case SG_BACKEND_VULKAN:
            name = "Vulkan";
            break;
        case SG_BACKEND_DUMMY:
            name = "dummy";
            break;
        }
        ImGui::Text("Renderer: %s", name);
    }
    ImGui::Text("FPS: %.1f", fps);
    ImGui::Text("Frame: %.2f ms  CPU submit: %.2f ms  Scene submit: %.2f ms", frame_interval_ms,
                render_submit_ms, scene_submit_ms);
    if constexpr (platform::kIsWeb) {
        if (ImGui::Button("Commit settings to URL")) {
            syncBrowserUrl();
        }
    }
    ImGui::Checkbox("throttle when unfocused", &cfg.pause_when_unfocused);
    if (!ibl_installed) {
        const auto frac = static_cast<float>(ibl_job.progress());
        ImGui::Text("IBL bake: %.0f%%", frac * 100.0f);
        ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f));
    }
    // Developer escape hatch: drop the persisted IBL cache. Doesn't touch
    // the live GPU IBL — only takes effect on the next page load / launch,
    // which then re-bakes from scratch.
    if (ImGui::Button("Clear IBL cache")) {
        clearIblCache();
    }
    if (!scene) {
        ImGui::Separator();
        // `show_drag_hint` flips off whenever the project has something
        // concrete to say (loading progress, ready/build status, or a
        // hard error) — otherwise we encourage the user to drop files.
        bool show_drag_hint = true;
        if (project_) {
            project_->poll();

            // Root selection is user-driven across all backends:
            // double-click in the tree panel sets the matching root
            // key. Initial roots can also come from external sources
            // (App::setRootKeys called by the URL JS shell or the CLI
            // entry point) — those just preselect the first build;
            // the tree click handler still treats them as overridable.

            // Drive the session: walk includes, parse config, import geometry.
            build_session.poll(project_.get());

            // Surface any project-level error first; build session errors
            // are surfaced via build_error below (set in the build_in_progress
            // branch when the build job completes with diagnostics).
            if (project_->status() == ProjectFsStatus::Error) {
                ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "Asset load failed:");
                ImGui::TextWrapped("%s", project_->errorMessage().c_str());
                show_drag_hint = false;
            }

            // Hand session inputs to the build job once they're ready.
            if (!build_in_progress && build_session.phase() == BuildPhase::ResolvedReady) {
                if (auto inputs = build_session.takeInputs()) {
                    build_start_time = std::chrono::steady_clock::now();
                    build_job.start(std::move(inputs->config.config),
                                    std::move(inputs->import.scene), std::move(inputs->config_key),
                                    std::move(inputs->geometry_key));
                    build_in_progress = true;
                    show_drag_hint = false;
                }
            }

            // Build progress UI (when the job is running).
            if (build_in_progress) {
                show_drag_hint = false;
                switch (build_job.phase()) {
                case SceneBuildJob::Phase::Preparing:
                    ImGui::Text("Loading config and importing geometry…");
                    break;
                case SceneBuildJob::Phase::Tessellating: {
                    const auto total = build_job.tessellationTotal();
                    const auto processed = build_job.tessellationProcessed();
                    if (total > 0) {
                        ImGui::Text("Tessellating… (%zu / %zu nodes)", processed, total);
                        const float frac =
                            static_cast<float>(processed) / static_cast<float>(total);
                        ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f));
                    } else {
                        ImGui::Text("Tessellating…");
                    }
                    break;
                }
                case SceneBuildJob::Phase::Finalizing:
                    ImGui::Text("Finalising scene…");
                    break;
                case SceneBuildJob::Phase::Idle:
                case SceneBuildJob::Phase::Done:
                    break;
                }
            }
            if (!project_->list("").empty()) {
                show_drag_hint = false;
            }

            // What the build session is waiting on (missing includes,
            // session-level errors).
            if (build_session.phase() == BuildPhase::WaitingForUser) {
                show_drag_hint = false;
                for (const auto &k : build_session.missing()) {
                    ImGui::Text("Still need: %s", k.c_str());
                }
            } else if (build_session.phase() == BuildPhase::Error) {
                show_drag_hint = false;
                ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "Build session error:");
                ImGui::TextWrapped("%s", build_session.errorMessage().c_str());
            }

            // App-level "still need a config" / "still need geometry"
            // hint, shown when the project has files but the user
            // hasn't picked a root yet (either via tree double-click
            // or via external setRootKeys).
            if (!project_->progress().empty()) {
                if (root_config_key.empty()) {
                    ImGui::Text("Pick a .toml config in the tree above");
                }
                if (root_geometry_key.empty()) {
                    ImGui::Text("Pick a FlatBuffer geometry (.nhb / .nhb.zst) in the tree above");
                }
            }

            for (const auto &w : project_->warnings()) {
                ImGui::TextColored({0.9f, 0.85f, 0.4f, 1.f}, "%s", w.c_str());
            }
        }
        if (!build_error.empty()) {
            ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", build_error.c_str());
        }
        if (show_drag_hint) {
            ImGui::Text("Drag a config (.toml) and a geometry file onto the window,");
            ImGui::Text("or use the Open files button below.");
        }
        if (ImGui::Button("Open files…")) {
            // Web dispatches inline (browser requires input.click() in
            // the gesture stack); native queues a latch and drains at
            // end of frame to avoid NFD re-entering the active ImGui
            // frame. Both shapes hide behind Platform::openFilePicker.
            platform_->openFilePicker();
        }
        // "Open folder…" mounts a real on-disk directory as the project
        // (FilesystemProjectFs). Native-only — the browser sandbox has
        // no folder picker via NFD; web users would need <input
        // webkitdirectory>, which is a Stage 4+ concern.
        if constexpr (!platform::kIsWeb) {
            ImGui::SameLine();
            if (ImGui::Button("Open folder…")) {
                platform_->openFolderPicker();
            }
        }
    } else if (scene && !scene_uploaded) {
        ImGui::Separator();
        ImGui::Text("Uploading scene to GPU…");
        ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f), "");
    }

    if (scene) {
        ImGui::Separator();
        ImGui::Text("Meshes: %u", scene_renderer.meshAssetCount());
        ImGui::Text("Nodes: %u", scene_renderer.nodeCount());
        ImGui::Text("Tris (scene): %llu",
                    static_cast<unsigned long long>(scene_renderer.triangleCount()));
        const auto fs = scene_renderer.lastFrameStats();
        ImGui::Text("Draw calls: %u  Instances: %u  Tris/frame: %llu", fs.draw_calls, fs.instances,
                    static_cast<unsigned long long>(fs.triangles));
        if (ImGui::Button("Frame scene")) {
            glm::vec3 bmin{0.f}, bmax{0.f};
            if (scene_renderer.worldBounds(bmin, bmax)) {
                scene_radius = camera.frameBounds(bmin, bmax);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close project")) {
            // Drop everything: the rendered scene, any accumulated bag
            // contents, any in-flight URL backend. Re-allocating a fresh
            // BagProjectFs gets us back to the App's startup shape, so
            // the user's next drop / pick starts a clean accumulation.
            scene_renderer.clearScene();
            scene.reset();
            project_ = std::make_unique<BagProjectFs>();
            root_config_key.clear();
            root_geometry_key.clear();
            build_session.setRootKeys({}, {});
            scene_uploaded = false;
            camera_framed = false;
            scene_radius = 0.f;
            build_error.clear();
        }
        ImGui::Separator();
        ImGui::Checkbox("backface cull", &cfg.cull_back);
        ImGui::Checkbox("auto orbit", &cfg.auto_orbit);
        ImGui::SliderFloat("orbit speed", &cfg.auto_orbit_speed_deg, -90.f, 90.f, "%.1f deg/s");
        ImGui::Checkbox("angle cut", &cfg.angle_cut);
        ImGui::Checkbox("shader angle cut", &cfg.shader_angle_cut);
        ImGui::SliderFloat("cut start", &cfg.angle_cut_start_deg, 0.f, 360.f, "%.1f deg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::InputFloat("##cut_start_input", &cfg.angle_cut_start_deg, 1.f, 15.f, "%.1f")) {
            cfg.angle_cut_start_deg = wrapDegrees(cfg.angle_cut_start_deg);
        }
        ImGui::SliderFloat("cut end", &cfg.angle_cut_end_deg, 0.f, 360.f, "%.1f deg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::InputFloat("##cut_end_input", &cfg.angle_cut_end_deg, 1.f, 15.f, "%.1f")) {
            cfg.angle_cut_end_deg = wrapDegrees(cfg.angle_cut_end_deg);
        }
        ImGui::Separator();
        ImGui::Checkbox("PBR / IBL", &cfg.enable_pbr);
        ImGui::Text("Camera: yaw=%.1f° pitch=%.1f° dist=%.2f", glm::degrees(camera.yaw),
                    glm::degrees(camera.pitch), camera.distance);
        ImGui::Text("        near=%.3f far=%.1f", camera.near_plane, camera.far_plane);
    }
    // Project panel — outside the scene-gated block so it stays
    // visible during and after a build. The tree view is the unified
    // project-presentation: every backend overrides list() to expose
    // its files (real hierarchy for filesystem / future archive,
    // flat for bag and URL). Double-clicking a recognised file
    // (.toml / .nhb / .nhb.zst) sets the matching root key — the
    // BuildSession picks it up on the next poll.
    if (project_) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Project", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char *status_label = "?";
            switch (project_->status()) {
            case ProjectFsStatus::Idle:
                status_label = "idle";
                break;
            case ProjectFsStatus::Fetching:
                status_label = "fetching";
                break;
            case ProjectFsStatus::Ready:
                status_label = "ready";
                break;
            case ProjectFsStatus::Error:
                status_label = "error";
                break;
            }
            const auto bname = project_->name();
            ImGui::Text("backend: %.*s   status: %s", static_cast<int>(bname.size()), bname.data(),
                        status_label);

            const auto root_nodes = project_->list("");
            if (root_nodes.empty()) {
                ImGui::TextDisabled("(no files yet)");
            } else {
                const auto selected_config_key = projectTreeSelectionKey(root_config_key);
                const auto selected_geometry_key = projectTreeSelectionKey(root_geometry_key);
                // Index progress() by key so each leaf can render a
                // fetch-state badge (in-flight URL, [ok], [fail])
                // regardless of backend.
                std::unordered_map<std::string_view, const ProjectProgress *> progress_by_key;
                for (const auto &p : project_->progress()) {
                    progress_by_key.emplace(p.url, &p);
                }
                auto isFlatBufferGeom = [](std::string_view key) {
                    auto path = std::filesystem::path{key};
                    auto ext = path.extension().string();
                    for (auto &c : ext) {
                        if (c >= 'A' && c <= 'Z') {
                            c = static_cast<char>(c - 'A' + 'a');
                        }
                    }
                    if (ext == ".nhb") {
                        return true;
                    }
                    if (ext != ".zst") {
                        return false;
                    }
                    auto stemExt = path.stem().extension().string();
                    for (auto &c : stemExt) {
                        if (c >= 'A' && c <= 'Z') {
                            c = static_cast<char>(c - 'A' + 'a');
                        }
                    }
                    return stemExt == ".nhb";
                };
                // ImGui::BeginChild("project_tree", ImVec2(0.f, 240.f), ImGuiChildFlags_Borders);
                // Deducing-this recursive lambda — avoids std::function
                // and its type-erased indirection. PushID per node
                // gives every TreeNodeEx call a fully isolated ID
                // scope, so two directories sharing a basename can't
                // alias their open/closed state regardless of how the
                // label hashes.
                auto renderNodes = [&](this const auto &self,
                                       std::span<const DirNode> nodes) -> void {
                    for (const auto &n : nodes) {
                        ImGui::PushID(n.key.c_str());
                        if (n.is_directory) {
                            const bool open = ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_None,
                                                                "%s", n.name.c_str());
                            if (open) {
                                self(n.children);
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                            continue;
                        }
                        // Fetch-state badge from progress(), when we
                        // have one for this key.
                        if (auto it = progress_by_key.find(n.key); it != progress_by_key.end()) {
                            const auto &p = *it->second;
                            if (p.failed) {
                                ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "[fail]");
                                ImGui::SameLine();
                            } else if (!p.done && p.bytes_total > 0) {
                                const float frac =
                                    static_cast<float>(static_cast<double>(p.bytes_done) /
                                                       static_cast<double>(p.bytes_total));
                                ImGui::ProgressBar(frac, ImVec2(60.f, 0.f));
                                ImGui::SameLine();
                            } else if (!p.done) {
                                ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()),
                                                   ImVec2(60.f, 0.f), "");
                                ImGui::SameLine();
                            }
                        }
                        const auto tree_key = projectTreeSelectionKey(n.key);
                        const bool is_config =
                            !selected_config_key.empty() && tree_key == selected_config_key;
                        const bool is_geom =
                            !selected_geometry_key.empty() && tree_key == selected_geometry_key;
                        ImGuiTreeNodeFlags flags =
                            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        if (is_config || is_geom) {
                            flags |= ImGuiTreeNodeFlags_Selected;
                        }
                        const char *tag = is_config ? " [config]" : is_geom ? " [geometry]" : "";
                        ImGui::TreeNodeEx("##leaf", flags, "%s%s", n.name.c_str(), tag);
                        // Double-click triggers auto-detection by
                        // extension and assigns the matching root key.
                        // Single-click is a no-op (avoids accidental
                        // rebuilds while exploring).
                        if (ImGui::IsItemHovered() &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            auto ext = std::filesystem::path{n.key}.extension().string();
                            for (auto &c : ext) {
                                if (c >= 'A' && c <= 'Z') {
                                    c = static_cast<char>(c - 'A' + 'a');
                                }
                            }
                            if (ext == ".toml") {
                                root_config_key = n.key;
                                build_session.setRootKeys(root_config_key, root_geometry_key);
                                build_error.clear();
                            } else if (isFlatBufferGeom(n.key)) {
                                root_geometry_key = n.key;
                                build_session.setRootKeys(root_config_key, root_geometry_key);
                                build_error.clear();
                            }
                        }
                        ImGui::PopID();
                    }
                };
                renderNodes(root_nodes);
                // ImGui::EndChild();
                if (ImGui::Button("Rescan")) {
                    // No-op on backends without a mutable source
                    // (bag, URL); meaningful for filesystem mounts
                    // where on-disk edits warrant a re-walk.
                    project_->rescan();
                    build_error.clear();
                }
            }
        }
    }
    ImGui::End();

    if (platform_window_state.drag_hover.active) {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const ImVec2 min = viewport->Pos;
        const ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
        auto *draw_list = ImGui::GetForegroundDrawList();
        draw_list->AddRectFilled(min, max, IM_COL32(80, 120, 180, 40));
        draw_list->AddRect(min, max, IM_COL32(130, 180, 255, 180), 0.f, 0, 3.f);

        const char *message = platform_window_state.drag_hover.file_like
                                  ? "Drop files to load them"
                                  : "Drop supported scene files";
        const ImVec2 text_size = ImGui::CalcTextSize(message);
        const ImVec2 center{(min.x + max.x - text_size.x) * 0.5f,
                            (min.y + max.y - text_size.y) * 0.5f};
        draw_list->AddText({center.x + 1.f, center.y + 1.f}, IM_COL32(0, 0, 0, 180), message);
        draw_list->AddText(center, IM_COL32(230, 240, 255, 255), message);
    }

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
}

void App::Impl::onCleanup() {
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
    // Reset session-side state so the BuildSession doesn't keep walking
    // an old project's stale keys against a new backend. Root keys
    // either come back via setRootKeys (URL mode) or via App-side
    // recognition on the next generation bump (bag mode).
    impl_->root_config_key.clear();
    impl_->root_geometry_key.clear();
    impl_->build_session.setRootKeys({}, {});
}

void App::setRootKeys(std::string config_key, std::string geometry_key) {
    impl_->root_config_key = config_key;
    impl_->root_geometry_key = geometry_key;
    impl_->build_session.setRootKeys(std::move(config_key), std::move(geometry_key));
}

ProjectFs *App::project() const noexcept { return impl_->project_.get(); }

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

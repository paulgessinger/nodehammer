#include <nodehammer/viewer/app.hpp>

#include "imgui_backend.hpp"

#include <nodehammer/ir/render.hpp>
#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/scene_renderer.hpp>

#include <imgui.h>
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>
#include <sokol_time.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

EM_JS(void, nh_viewer_commit_url_state, (const char *state_query, const char *managed_keys), {
    var url = new URL(window.location.href);
    var p = url.searchParams;
    var keys = UTF8ToString(managed_keys).split(',');
    for (var i = 0; i < keys.length; ++i) {
        if (keys[i]) {
            p.delete(keys[i]);
        }
    }

    var state = new URLSearchParams(UTF8ToString(state_query));
    state.forEach(function(value, key) { p.set(key, value); });

    history.replaceState(null, "", url);
});
#endif

namespace nodehammer::viewer {

namespace {

float wrap_degrees(float angle) {
    angle = std::fmod(angle, 360.f);
    if (angle < 0.f) {
        angle += 360.f;
    }
    return angle;
}

std::string format_url_float(float value) {
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

void append_url_param(std::string &query, std::string_view name, std::string_view value) {
    if (!query.empty()) {
        query.push_back('&');
    }
    query.append(name);
    query.push_back('=');
    query.append(value);
}

void append_url_bool(std::string &query, std::string_view name, bool value, bool default_value) {
    if (value != default_value) {
        append_url_param(query, name, value ? "1" : "0");
    }
}

void append_url_float(std::string &query, std::string_view name, float value, float default_value) {
    if (std::abs(value - default_value) >= 0.0001f) {
        append_url_param(query, name, format_url_float(value));
    }
}

} // namespace

struct App::Impl {
    Config cfg;
    bool quit{false};

    std::shared_ptr<const RenderScene> scene;
    SceneRenderer scene_renderer;
    Camera camera;
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

    explicit Impl(Config c) : cfg(std::move(c)) {}

    void on_init();
    void on_frame();
    void on_event(const sapp_event *ev);
    void on_cleanup();

    void update_camera_input();
    void apply_initial_camera();
    void render();
    void sync_browser_url() const;
    [[nodiscard]] std::string browser_url_state_query() const;

    static void init_cb(void *user) { static_cast<Impl *>(user)->on_init(); }
    static void frame_cb(void *user) { static_cast<Impl *>(user)->on_frame(); }
    static void event_cb(const sapp_event *ev, void *user) {
        static_cast<Impl *>(user)->on_event(ev);
    }
    static void cleanup_cb(void *user) { static_cast<Impl *>(user)->on_cleanup(); }
};

void App::Impl::on_init() {
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
}

void App::Impl::on_event(const sapp_event *ev) {
    ImGui_ImplSokol_HandleEvent(ev);
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
    }
}

void App::Impl::update_camera_input() {
    if (!scene) {
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
    }
    if (io.MouseWheel != 0.f) {
        // 1.1^wheel: each notch = 10% closer/further. Matches Blender feel.
        camera.dolly(std::pow(1.1f, -io.MouseWheel));
    }
}

void App::Impl::apply_initial_camera() {
    if (!cfg.initial_camera.has_value()) {
        return;
    }
    const float scene_radius = camera.scene_radius;
    camera = *cfg.initial_camera;
    camera.scene_radius = scene_radius;
    camera.pitch = std::clamp(camera.pitch, -1.553343f, 1.553343f);
    camera.dolly(1.f);
}

void App::Impl::render() {
    const uint64_t render_submit_start = stm_now();
    scene_submit_ms = 0.0;

    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = {0.125f, 0.157f, 0.188f, 1.0f}; // 0x202830
    pass.action.depth.load_action = SG_LOADACTION_CLEAR;
    // Reversed-Z: clear to 0 (the "farthest" depth in our convention).
    pass.action.depth.clear_value = 0.0f;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);

    if (scene) {
        if (!scene_uploaded) {
            scene_renderer.upload(*scene);
            scene_uploaded = true;
        }
        if (!camera_framed) {
            glm::vec3 bmin{0.f}, bmax{0.f};
            if (scene_renderer.world_bounds(bmin, bmax)) {
                camera.frame_bounds(bmin, bmax);
                apply_initial_camera();
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

std::string App::Impl::browser_url_state_query() const {
    std::string query;
    append_url_bool(query, "cullBack", cfg.cull_back, true);
    append_url_bool(query, "pauseWhenUnfocused", cfg.pause_when_unfocused, true);
    append_url_bool(query, "autoOrbit", cfg.auto_orbit, false);
    append_url_float(query, "orbitSpeed", cfg.auto_orbit_speed_deg, 15.f);
    append_url_bool(query, "angleCut", cfg.angle_cut, false);
    append_url_bool(query, "shaderAngleCut", cfg.shader_angle_cut, true);
    append_url_float(query, "cutStart", cfg.angle_cut_start_deg, 0.f);
    append_url_float(query, "cutEnd", cfg.angle_cut_end_deg, 90.f);
    append_url_bool(query, "pbr", cfg.enable_pbr, false);
    append_url_param(query, "cameraTargetX", format_url_float(camera.target.x));
    append_url_param(query, "cameraTargetY", format_url_float(camera.target.y));
    append_url_param(query, "cameraTargetZ", format_url_float(camera.target.z));
    append_url_param(query, "cameraDistance", format_url_float(camera.distance));
    append_url_param(query, "cameraYaw", format_url_float(glm::degrees(camera.yaw)));
    append_url_param(query, "cameraPitch", format_url_float(glm::degrees(camera.pitch)));
    return query;
}

void App::Impl::sync_browser_url() const {
#ifdef __EMSCRIPTEN__
    const std::string state_query = browser_url_state_query();
    constexpr const char *managed_keys =
        "cullBack,pauseWhenUnfocused,autoOrbit,orbitSpeed,angleCut,shaderAngleCut,cutStart,cutEnd,"
        "pbr,cameraTargetX,cameraTargetY,cameraTargetZ,cameraDistance,cameraYaw,cameraPitch";
    nh_viewer_commit_url_state(state_query.c_str(), managed_keys);
#endif
}

void App::Impl::on_frame() {
    if (cfg.pause_when_unfocused && (!window_focused || !window_visible)) {
        last_time = stm_now();
        fps_window_start = last_time;
        frame_count = 0;
        delta_seconds = 0.0;
        return;
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

    update_camera_input();
    if (scene && cfg.auto_orbit) {
        camera.orbit(glm::radians(cfg.auto_orbit_speed_deg) * static_cast<float>(delta_seconds),
                     0.f);
    }

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
#ifdef __EMSCRIPTEN__
    if (ImGui::Button("Commit settings to URL")) {
        sync_browser_url();
    }
#endif
    ImGui::Checkbox("pause when unfocused", &cfg.pause_when_unfocused);
    if (scene) {
        ImGui::Separator();
        ImGui::Text("Meshes: %u", scene_renderer.mesh_asset_count());
        ImGui::Text("Nodes: %u", scene_renderer.node_count());
        ImGui::Text("Tris (scene): %llu",
                    static_cast<unsigned long long>(scene_renderer.triangle_count()));
        const auto fs = scene_renderer.last_frame_stats();
        ImGui::Text("Draw calls: %u  Instances: %u  Tris/frame: %llu", fs.draw_calls, fs.instances,
                    static_cast<unsigned long long>(fs.triangles));
        if (ImGui::Button("Frame scene")) {
            glm::vec3 bmin{0.f}, bmax{0.f};
            if (scene_renderer.world_bounds(bmin, bmax)) {
                camera.frame_bounds(bmin, bmax);
            }
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
            cfg.angle_cut_start_deg = wrap_degrees(cfg.angle_cut_start_deg);
        }
        ImGui::SliderFloat("cut end", &cfg.angle_cut_end_deg, 0.f, 360.f, "%.1f deg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::InputFloat("##cut_end_input", &cfg.angle_cut_end_deg, 1.f, 15.f, "%.1f")) {
            cfg.angle_cut_end_deg = wrap_degrees(cfg.angle_cut_end_deg);
        }
        ImGui::Separator();
        ImGui::Checkbox("PBR / IBL", &cfg.enable_pbr);
        ImGui::Text("Camera: yaw=%.1f° pitch=%.1f° dist=%.2f", glm::degrees(camera.yaw),
                    glm::degrees(camera.pitch), camera.distance);
        ImGui::Text("        near=%.3f far=%.1f", camera.near_plane, camera.far_plane);
    } else {
        ImGui::Text("(no scene loaded)");
    }
    ImGui::End();

    // simgui_render (called from inside render() → ImGui_ImplSokol_Render)
    // internally calls ImGui::Render itself before issuing draws.
    render();
}

void App::Impl::on_cleanup() {
    scene_renderer.release();
    // simgui_shutdown destroys the ImGui context — don't call
    // ImGui::DestroyContext separately (double-free crash on macOS quit).
    ImGui_ImplSokol_Shutdown();
    sg_shutdown();
}

void App::set_scene(std::shared_ptr<const RenderScene> scene) {
    impl_->scene = std::move(scene);
    impl_->scene_uploaded = false;
    impl_->camera_framed = false;
}

App::App(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

App::~App() = default;

int App::run() {
    sapp_desc desc{};
    desc.user_data = impl_.get();
    desc.init_userdata_cb = &Impl::init_cb;
    desc.frame_userdata_cb = &Impl::frame_cb;
    desc.event_userdata_cb = &Impl::event_cb;
    desc.cleanup_userdata_cb = &Impl::cleanup_cb;
    desc.width = static_cast<int>(impl_->cfg.width);
    desc.height = static_cast<int>(impl_->cfg.height);
    desc.window_title = impl_->cfg.title.c_str();
    desc.high_dpi = true;
    desc.swap_interval = impl_->cfg.vsync ? 1 : 0;
    // Default canvas selector for emscripten: matches the <canvas id="canvas">
    // in web/viewer.html.
    desc.html5.canvas_selector = "#canvas";
    desc.logger.func = slog_func;
    // Tell sokol_app it owns the main loop on emscripten too — sapp_run
    // internally calls emscripten_set_main_loop and unwinds the stack via
    // its async exception, mirroring the bgfx App's emscripten_set_main_loop_arg
    // call. The Impl pointer must outlive that unwind, so detach it: the
    // wasm runtime takes ownership for the page lifetime.
#ifdef __EMSCRIPTEN__
    impl_.release();
#endif
    sapp_run(&desc);
    return 0;
}

} // namespace nodehammer::viewer

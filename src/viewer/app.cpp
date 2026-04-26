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

#include <cmath>
#include <memory>
#include <utility>

namespace nodehammer::viewer {

namespace {

float wrap_degrees(float angle) {
    angle = std::fmod(angle, 360.f);
    if (angle < 0.f) {
        angle += 360.f;
    }
    return angle;
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

    bool wireframe{false};
    bool cull_back{true};
    bool pause_when_unfocused{true};
    bool window_focused{true};
    bool window_visible{true};
    bool auto_orbit{false};
    float auto_orbit_speed_deg{15.f};
    bool angle_cut{false};
    bool shader_angle_cut{true};
    float angle_cut_start_deg{0.f};
    float angle_cut_end_deg{90.f};

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
    void render();

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
                camera_framed = true;
            }
        }
        SceneRenderer::RenderFlags flags;
        flags.wireframe = wireframe;
        flags.cull_back = cull_back;
        flags.angle_cut = angle_cut;
        flags.shader_angle_cut = shader_angle_cut;
        flags.angle_cut_start_deg = angle_cut_start_deg;
        flags.angle_cut_end_deg = angle_cut_end_deg;
        const uint64_t scene_submit_start = stm_now();
        scene_renderer.render(camera, fb_width, fb_height, flags);
        scene_submit_ms = stm_sec(stm_diff(stm_now(), scene_submit_start)) * 1000.0;
    }

    ImGui_ImplSokol_Render();

    sg_end_pass();
    sg_commit();
    render_submit_ms = stm_sec(stm_diff(stm_now(), render_submit_start)) * 1000.0;
}

void App::Impl::on_frame() {
    if (pause_when_unfocused && (!window_focused || !window_visible)) {
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
    if (scene && auto_orbit) {
        camera.orbit(glm::radians(auto_orbit_speed_deg) * static_cast<float>(delta_seconds), 0.f);
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
    ImGui::Checkbox("pause when unfocused", &pause_when_unfocused);
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
        ImGui::Checkbox("backface cull", &cull_back);
        ImGui::Checkbox("auto orbit", &auto_orbit);
        ImGui::SliderFloat("orbit speed", &auto_orbit_speed_deg, -90.f, 90.f, "%.1f deg/s");
        ImGui::Checkbox("angle cut", &angle_cut);
        ImGui::Checkbox("shader angle cut", &shader_angle_cut);
        ImGui::SliderFloat("cut start", &angle_cut_start_deg, 0.f, 360.f, "%.1f deg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::InputFloat("##cut_start_input", &angle_cut_start_deg, 1.f, 15.f, "%.1f")) {
            angle_cut_start_deg = wrap_degrees(angle_cut_start_deg);
        }
        ImGui::SliderFloat("cut end", &angle_cut_end_deg, 0.f, 360.f, "%.1f deg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::InputFloat("##cut_end_input", &angle_cut_end_deg, 1.f, 15.f, "%.1f")) {
            angle_cut_end_deg = wrap_degrees(angle_cut_end_deg);
        }
        (void)wireframe;
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

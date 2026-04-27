#include <nodehammer/viewer/app.hpp>

#include "ibl.hpp"
#include "imgui_backend.hpp"
#include "scene_build_job.hpp"
#include "scene_renderer.hpp"

#include <nodehammer/ir/render.hpp>
#include <nodehammer/scene_build.hpp>
#include <nodehammer/viewer/asset_source.hpp>
#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/local_file_asset_source.hpp>

#ifdef NH_HAS_NFD
#include <nfd.hpp>
#endif

#include <imgui.h>
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>
#include <sokol_time.h>

#include <chrono>
#include <cmath>
#include <iomanip>
#include <memory>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <sys/stat.h>

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

// Open a transient <input type=file multiple> picker. For each chosen file,
// read its bytes via FileReader and call back into wasm at
// `_nh_viewer_deliver_upload(name_ptr, data_ptr, size)`. Filename is
// UTF-8-encoded via TextEncoder to avoid depending on emscripten's
// stringToNewUTF8 helper being in EXPORTED_RUNTIME_METHODS.
EM_JS(void, nh_viewer_open_file_picker, (), {
    var input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.accept = '.toml,.nhb,.zst,.gltf,.glb,.gdml,.root,.fb,.json,.xml';
    input.style.display = 'none';
    input.addEventListener(
        'change', function(ev) {
            var files = ev.target.files;
            for (var i = 0; i < files.length; ++i) {
                (function(f) {
                    var reader = new FileReader();
                    reader.onload = function() {
                        var bytes = new Uint8Array(reader.result);
                        var size = bytes.length;
                        var data_ptr = Module._malloc(size);
                        Module.HEAPU8.set(bytes, data_ptr);

                        var enc = new TextEncoder();
                        var name_utf8 = enc.encode(f.name);
                        var name_ptr = Module._malloc(name_utf8.length + 1);
                        Module.HEAPU8.set(name_utf8, name_ptr);
                        Module.HEAPU8[name_ptr + name_utf8.length] = 0;

                        Module._nh_viewer_deliver_upload(name_ptr, data_ptr, size);

                        Module._free(name_ptr);
                        Module._free(data_ptr);
                    };
                    reader.readAsArrayBuffer(f);
                })(files[i]);
            }
            document.body.removeChild(input);
        });
    document.body.appendChild(input);
    input.click();
});
#else
void nh_viewer_commit_url_state(const char *state_query, const char *managed_keys);
#endif

namespace nodehammer::viewer {

namespace {

static constexpr bool kIsEmscripten =
#ifdef __EMSCRIPTEN__
    true;
#else
    false;
#endif

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
    std::unique_ptr<AssetSource> source;
    SceneRenderer scene_renderer;
    Camera camera;

    // Procedural IBL bake. Started right after sg_setup so we don't pay
    // the cost on first scene-load. Native runs the bake on a worker
    // thread; web time-slices it on the main thread inside `advance`.
    IblBakeJob ibl_job;
    bool ibl_installed{false};
    std::chrono::steady_clock::time_point ibl_start_time{};

    // Off-loop scene tessellation. Native runs the build on a worker
    // thread so the UI stays smooth. Web defers the synchronous build by
    // one frame so the previous frame paints a "Tessellating…" message
    // before the page freezes.
    SceneBuildJob build_job;
    bool build_in_progress{false};
    std::chrono::steady_clock::time_point build_start_time{};

#ifdef __EMSCRIPTEN__
    // Web upload glue (file picker EM_JS shim and the drop-bytes fetch
    // callback) need a way to call back into the App from JS / sokol.
    // There is exactly one App alive per page lifetime, so a single static
    // pointer suffices. on_init sets it; on_cleanup clears it.
    static Impl *active;

    // Materialise an in-memory file into MEMFS at /uploads/<filename>,
    // then route it through the LocalFileAssetSource. Auto-creates a
    // LocalFileAssetSource if none is currently set so a fresh page that
    // started in upload-only mode can still receive a drop.
    void deliver_upload(const std::string &filename, const std::uint8_t *data, std::size_t size);
#endif

    // Stashed message after a build failure (so the UI can keep showing it
    // for more than the one frame the source survives).
    std::string build_error;

    // True when the imgui "Open files…" button was clicked this frame.
    // The actual NFD modal must run AFTER the imgui frame finishes — it
    // enters a nested Cocoa runloop that fires sokol's display link while
    // the modal is up, which would re-enter on_frame and double up
    // ImGui::NewFrame / ImGui::Begin (corrupting the outer frame's window
    // stack and crashing in the next ImGui::End). Deferring to after
    // render() means any re-entries land cleanly between frames.
    bool open_picker_requested{false};
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

    // Kick off the IBL bake immediately. The renderer is initialised with
    // 1×1 placeholder IBL textures so it can render before the bake
    // completes; on_frame swaps in the real result when ibl_job.advance
    // returns true.
    scene_renderer.initialize();
    ibl_start_time = std::chrono::steady_clock::now();
    ibl_job.start();

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

#ifdef __EMSCRIPTEN__
    Impl::active = this;
#endif
}

#ifdef __EMSCRIPTEN__

App::Impl *App::Impl::active = nullptr;

namespace {

// Per-fetch context for sokol's async dropped-file byte fetch on web.
// sokol writes the file's bytes into `buffer` and then invokes the
// callback once the FileReader resolves; we keep the buffer alive until
// then by heap-allocating this struct.
struct WebDropCtx {
    std::vector<std::uint8_t> buffer;
    std::string filename;
    App::Impl *self;
};

void web_drop_fetch_callback(const sapp_html5_fetch_response *response) {
    auto *ctx = static_cast<WebDropCtx *>(response->user_data);
    if (response->succeeded) {
        ctx->self->deliver_upload(ctx->filename, ctx->buffer.data(),
                                  static_cast<std::size_t>(response->data.size));
    } else {
        std::println(stderr, "viewer: failed to fetch dropped file '{}'", ctx->filename);
    }
    delete ctx;
}

void make_uploads_dir() {
    // EEXIST is the success case for re-runs.
    ::mkdir("/uploads", 0755);
}

} // namespace

void App::Impl::deliver_upload(const std::string &filename, const std::uint8_t *data,
                               std::size_t size) {
    if (!source) {
        source = std::make_unique<LocalFileAssetSource>();
        scene.reset();
        scene_uploaded = false;
        camera_framed = false;
        build_error.clear();
    }
    make_uploads_dir();
    const std::string path = "/uploads/" + filename;
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::println(stderr, "viewer: failed to open MEMFS path '{}' for writing", path);
        return;
    }
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok) {
        std::println(stderr, "viewer: short write delivering '{}'", path);
        return;
    }
    source->ingest_local_file(std::filesystem::path{path});
}

#endif // __EMSCRIPTEN__

extern "C" {

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void nh_viewer_deliver_upload(const char *filename, const std::uint8_t *data, std::size_t size) {
    if (App::Impl::active == nullptr || filename == nullptr) {
        return;
    }
    App::Impl::active->deliver_upload(filename, data, size);
}
#endif

} // extern "C"

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
    } else if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        // If no source is currently set, spin one up so the dropped file
        // has somewhere to land. This handles the "drop a scene to load it"
        // flow even when the App was started with no CLI args.
        if (!source) {
            source = std::make_unique<LocalFileAssetSource>();
            scene.reset();
            scene_uploaded = false;
            camera_framed = false;
            build_error.clear();
        }
        const int n = sapp_get_num_dropped_files();
        for (int i = 0; i < n; ++i) {
#ifdef __EMSCRIPTEN__
            // On web, sapp_get_dropped_file_path returns just the filename
            // (no real path) and the bytes have to be fetched asynchronously
            // via FileReader. Allocate a buffer of the right size, kick off
            // the fetch, and route the bytes through deliver_upload when
            // the callback fires.
            const std::uint64_t size = sapp_html5_get_dropped_file_size(i);
            auto *ctx = new WebDropCtx;
            ctx->filename = sapp_get_dropped_file_path(i);
            ctx->buffer.resize(static_cast<std::size_t>(size));
            ctx->self = this;
            sapp_html5_fetch_request req{};
            req.dropped_file_index = i;
            req.callback = &web_drop_fetch_callback;
            req.buffer = sapp_range{ctx->buffer.data(), ctx->buffer.size()};
            req.user_data = ctx;
            sapp_html5_fetch_dropped_file(&req);
#else
            source->ingest_local_file(std::filesystem::path{sapp_get_dropped_file_path(i)});
#endif
        }
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
        camera.dolly(std::pow(1.1f, -io.MouseWheel), scene_radius);
    }
}

void App::Impl::apply_initial_camera() {
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
        if (!scene_renderer.upload_in_progress()) {
            scene_renderer.begin_upload(scene);
        }
        if (scene_renderer.advance_upload()) {
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
            if (scene_renderer.world_bounds(bmin, bmax)) {
                scene_radius = camera.frame_bounds(bmin, bmax);
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
    append_url_bool(query, "pbr", cfg.enable_pbr, true);
    append_url_param(query, "cameraTargetX", format_url_float(camera.target.x));
    append_url_param(query, "cameraTargetY", format_url_float(camera.target.y));
    append_url_param(query, "cameraTargetZ", format_url_float(camera.target.z));
    append_url_param(query, "cameraDistance", format_url_float(camera.distance));
    append_url_param(query, "cameraYaw", format_url_float(glm::degrees(camera.yaw)));
    append_url_param(query, "cameraPitch", format_url_float(glm::degrees(camera.pitch)));
    return query;
}

void App::Impl::sync_browser_url() const {
    if constexpr (kIsEmscripten) {
        const std::string state_query = browser_url_state_query();
        constexpr const char *managed_keys =
            "cullBack,pauseWhenUnfocused,autoOrbit,orbitSpeed,angleCut,shaderAngleCut,cutStart,"
            "cutEnd,"
            "pbr,cameraTargetX,cameraTargetY,cameraTargetZ,cameraDistance,cameraYaw,cameraPitch";
        nh_viewer_commit_url_state(state_query.c_str(), managed_keys);
    }
}

void App::Impl::on_frame() {
    // Drive the procedural IBL bake to completion. On native this is a
    // single-flag poll (the bake runs on a worker thread); on web this
    // spends up to ~8 ms doing pixel work on the main thread. We don't
    // gate this on focus: the bake should finish whether or not the user
    // is looking at the window.
    if (!ibl_installed && ibl_job.advance()) {
        scene_renderer.install_ibl(ibl_job.take());
        ibl_installed = true;
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - ibl_start_time)
                                    .count();
        std::println("viewer: IBL bake complete ({:.1f} ms)", elapsed_ms);
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
        source.reset();
    }

    // When unfocused, throttle to a low rate (~5 Hz) instead of pausing
    // entirely. Full pause was visible as "drag-and-drop into a
    // background viewer feels broken": the drop event fired and the
    // async source/scene-build state advanced, but no frame rendered the
    // result until focus returned. 5 Hz keeps drag-over feedback and
    // load progress visible while still cutting GPU cost ~12x vs. the
    // active 60 Hz path. The flag's URL/persistence name stays
    // `pauseWhenUnfocused` for backwards compatibility.
    if (cfg.pause_when_unfocused && (!window_focused || !window_visible)) {
        constexpr double kIdleFrameInterval = 0.2; // 5 Hz
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
    if constexpr (kIsEmscripten) {
        if (ImGui::Button("Commit settings to URL")) {
            sync_browser_url();
        }
    }
    ImGui::Checkbox("throttle when unfocused", &cfg.pause_when_unfocused);
    if (source && !scene) {
        ImGui::Separator();
        source->poll();
        const auto state = source->state();
        if (state == LoadState::Error) {
            ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "Asset load failed:");
            ImGui::TextWrapped("%s", source->error_message().c_str());
        } else if (state == LoadState::Ready) {
            // Hand the paths to the build job. Native immediately spawns
            // a worker thread; web defers the synchronous build by one
            // poll so this frame's "Tessellating…" UI paints first.
            if (!build_in_progress) {
                build_start_time = std::chrono::steady_clock::now();
                build_job.start(source->config_path(), source->input_path());
                build_in_progress = true;
            }
            ImGui::Text("Tessellating…");
            ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f), "");
        } else {
            // Idle / Fetching: render a source-shaped placeholder. URL-style
            // sources will populate `progress()` with active downloads;
            // accumulator-style sources (drag-and-drop / picker) populate
            // it with already-collected files.
            const auto entries = source->progress();
            if (entries.empty()) {
                ImGui::Text("Drag a config (.toml) and a geometry file onto the window,");
                ImGui::Text("or use the Open files button below.");
            } else {
                ImGui::Text("Loading…");
                for (const auto &p : entries) {
                    if (!p.done && p.bytes_total > 0) {
                        const float frac = static_cast<float>(static_cast<double>(p.bytes_done) /
                                                              static_cast<double>(p.bytes_total));
                        ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f));
                    } else if (!p.done) {
                        ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()),
                                           ImVec2(-1.f, 0.f), "");
                    } else {
                        ImGui::TextColored({0.5f, 0.9f, 0.5f, 1.f}, "[ok]");
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", p.url.c_str());
                }
            }
            // Accumulator-source-specific hints: list which slot is still
            // empty and surface any unrecognised filenames.
            if (auto *local = dynamic_cast<LocalFileAssetSource *>(source.get())) {
                if (local->needs_config()) {
                    ImGui::Text("Still waiting for: a .toml config");
                }
                if (local->needs_input()) {
                    ImGui::Text("Still waiting for: a geometry file (.nhb.zst, .gdml, .gltf, …)");
                }
                if (!local->last_unrecognised().empty()) {
                    ImGui::TextColored({1.f, 0.7f, 0.4f, 1.f}, "Don't know what to do with: %s",
                                       local->last_unrecognised().c_str());
                }
            }
            if (ImGui::Button("Open files…")) {
#ifdef NH_HAS_NFD
                // Native: defer to after render(). NFD's NSOpenPanel /
                // GTK / IFileDialog all enter nested event loops; running
                // them inside an active ImGui frame causes re-entry.
                open_picker_requested = true;
#elif defined(__EMSCRIPTEN__)
                // Web: the EM_JS shim returns immediately and the
                // FileReader callback fires later, so it's safe to call
                // inline (and the browser requires input.click() to run
                // from the user-gesture stack).
                nh_viewer_open_file_picker();
#endif
            }
        }
    } else if (!scene && !build_error.empty()) {
        ImGui::Separator();
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", build_error.c_str());
    } else if (scene && !scene_uploaded) {
        ImGui::Separator();
        ImGui::Text("Uploading scene to GPU…");
        ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f), "");
    }

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
                scene_radius = camera.frame_bounds(bmin, bmax);
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
    } else if (!source) {
        ImGui::Text("(no scene loaded)");
    }
    ImGui::End();

    // simgui_render (called from inside render() → ImGui_ImplSokol_Render)
    // internally calls ImGui::Render itself before issuing draws.
    render();

#ifdef NH_HAS_NFD
    // Deferred picker — see open_picker_requested above. By the time we
    // get here, the ImGui frame is fully ended and rendered, sokol's pass
    // is committed, and any sokol_app frame_cb re-entry from the modal's
    // nested run loop will run a clean self-contained frame instead of
    // overlapping with one already in progress.
    if (open_picker_requested && source) {
        open_picker_requested = false;
        NFD::Guard nfd;
        NFD::UniquePathSet picked;
        nfdu8filteritem_t filters[] = {
            {"Nodehammer scene", "toml,nhb,zst,gltf,glb,gdml,root,fb,json,xml"},
        };
        if (NFD::OpenDialogMultiple(picked, filters, 1) == NFD_OKAY) {
            nfdpathsetsize_t count = 0;
            NFD::PathSet::Count(picked, count);
            for (nfdpathsetsize_t i = 0; i < count; ++i) {
                NFD::UniquePathSetPathU8 path;
                if (NFD::PathSet::GetPath(picked, i, path) == NFD_OKAY) {
                    source->ingest_local_file(std::filesystem::path{path.get()});
                }
            }
        }
    }
#endif
}

void App::Impl::on_cleanup() {
    scene_renderer.release();
    // simgui_shutdown destroys the ImGui context — don't call
    // ImGui::DestroyContext separately (double-free crash on macOS quit).
    ImGui_ImplSokol_Shutdown();
    sg_shutdown();
#ifdef __EMSCRIPTEN__
    Impl::active = nullptr;
#endif
}

void App::set_scene(std::shared_ptr<const RenderScene> scene) {
    impl_->scene = std::move(scene);
    impl_->scene_uploaded = false;
    impl_->camera_framed = false;
}

void App::set_source(std::unique_ptr<AssetSource> source) {
    impl_->source = std::move(source);
    // Replacing a source mid-session clears the current scene so the
    // placeholder UI can take over while the new source resolves.
    impl_->scene.reset();
    impl_->scene_uploaded = false;
    impl_->camera_framed = false;
    impl_->build_error.clear();
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
    // Enable drag-and-drop file events. The buffer size needs to be at
    // least the longest path we'd ever see; 1 KiB is safe for typical
    // scene+config drops.
    desc.enable_dragndrop = true;
    desc.max_dropped_files = 8;
    desc.max_dropped_file_path_length = 1024;
    // Tell sokol_app it owns the main loop on emscripten too — sapp_run
    // internally calls emscripten_set_main_loop and unwinds the stack via
    // its async exception, mirroring the bgfx App's emscripten_set_main_loop_arg
    // call. The Impl pointer must outlive that unwind, so detach it: the
    // wasm runtime takes ownership for the page lifetime.
    if constexpr (kIsEmscripten) {
        impl_.release();
    }
    sapp_run(&desc);
    return 0;
}

} // namespace nodehammer::viewer

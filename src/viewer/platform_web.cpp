#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <emscripten/emscripten.h>
#include <sokol_app.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <vector>

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

// Open a transient <input type=file multiple> picker. After the user
// selects N files, we wait until every FileReader has resolved (Promise.all)
// and then push the whole batch through the C exports as one gesture:
//   begin → add × N → end
// Matches the drop-event model on the C side: one source per user gesture,
// installed on the App only once all bytes have landed. Filenames are
// UTF-8-encoded via TextEncoder to avoid depending on emscripten's
// stringToNewUTF8 helper being in EXPORTED_RUNTIME_METHODS.
// clang-format off
EM_JS(void, nh_viewer_open_file_picker, (), {
    var input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.accept = '.toml,.nhb,.zst,.gltf,.glb,.gdml,.root,.fb,.json,.xml';
    input.style.display = 'none';
    input.addEventListener(
        'change', async function(ev) {
            var files = Array.from(ev.target.files);
            document.body.removeChild(input);
            if (files.length === 0) {
                return;
            }
            var buffers = await Promise.all(files.map(function(f) { return f.arrayBuffer(); }));
            var handle = Module._nh_viewer_begin_upload_batch();
            var enc = new TextEncoder();
            for (var i = 0; i < files.length; ++i) {
                var bytes = new Uint8Array(buffers[i]);
                var size = bytes.length;
                var data_ptr = Module._malloc(size);
                Module.HEAPU8.set(bytes, data_ptr);

                var name_utf8 = enc.encode(files[i].name);
                var name_ptr = Module._malloc(name_utf8.length + 1);
                Module.HEAPU8.set(name_utf8, name_ptr);
                Module.HEAPU8[name_ptr + name_utf8.length] = 0;

                Module._nh_viewer_add_upload(handle, name_ptr, data_ptr, size);

                Module._free(name_ptr);
                Module._free(data_ptr);
            }
            Module._nh_viewer_end_upload_batch(handle);
        });
    document.body.appendChild(input);
    input.click();
});
// clang-format on

namespace nodehammer::viewer::platform {

namespace {

// Per-file fetch context. sokol writes the file's bytes into `buffer` then
// invokes the callback; we transfer ownership via `release()` into
// req.user_data and reclaim with a fresh unique_ptr in the callback. Each
// file's bytes flow straight into the App's long-lived project, so there
// is no per-gesture batch object to refcount — the App owns the project,
// the project lifetime survives any number of in-flight fetches.
struct WebDropCtx {
    std::vector<std::byte> buffer;
    std::string filename;
};

void webDropFetchCallback(const sapp_html5_fetch_response *response) {
    std::unique_ptr<WebDropCtx> ctx{static_cast<WebDropCtx *>(response->user_data)};
    std::println(stderr, "[viewer] drop callback: file='{}' succeeded={} size={}", ctx->filename,
                 response->succeeded, static_cast<std::size_t>(response->data.size));
    if (!response->succeeded) {
        std::println(stderr, "viewer: failed to fetch dropped file '{}'", ctx->filename);
        return;
    }
    auto *app = App::instance();
    if (app == nullptr) {
        std::println(stderr, "[viewer] drop callback: App::instance() is null");
        return;
    }
    auto *project = app->project();
    if (project == nullptr) {
        std::println(stderr, "[viewer] drop callback: app->project() is null");
        return;
    }
    project->addBytes(ctx->filename,
                      std::span<const std::byte>{ctx->buffer.data(),
                                                 static_cast<std::size_t>(response->data.size)});
}

} // namespace

/// Web platform state. Empty — the browser file picker dispatches
/// inline at button-click time, byte-fetch callbacks for drops route
/// through `App::instance()`, and there are no latches to hold. The
/// `app` back-reference is here for symmetry with the native impl in
/// case future web flows need it (e.g., a setProject call from a JS
/// shim that prefers a typed reference over the singleton).
struct Platform::Impl {
    App &app;
};

Platform::Platform(App &app) : impl_(std::make_unique<Impl>(app)) {}
Platform::~Platform() = default;

void Platform::dispatchDroppedFiles() {
    const int n = sapp_get_num_dropped_files();
    std::println(stderr, "[viewer] dispatchDroppedFiles: n={}", n);
    if (n == 0) {
        return;
    }
    // Bytes land asynchronously through `webDropFetchCallback`, which
    // reaches the App via `App::instance()` — sokol's fetch callbacks
    // are plain C function pointers and outlive any per-gesture
    // context, so we don't try to thread `impl_->app` through them.
    for (int i = 0; i < n; ++i) {
        const auto size = sapp_html5_get_dropped_file_size(i);
        auto ctx = std::make_unique<WebDropCtx>();
        ctx->filename = sapp_get_dropped_file_path(i);
        ctx->buffer.resize(static_cast<std::size_t>(size));
        sapp_html5_fetch_request req{};
        req.dropped_file_index = i;
        req.callback = &webDropFetchCallback;
        req.buffer = sapp_range{ctx->buffer.data(), ctx->buffer.size()};
        req.user_data = ctx.release();
        sapp_html5_fetch_dropped_file(&req);
    }
}

void Platform::commitUrlState(const std::string &state_query, const std::string &managed_keys) {
    nh_viewer_commit_url_state(state_query.c_str(), managed_keys.c_str());
}

void Platform::openFilePicker() { nh_viewer_open_file_picker(); }
void Platform::openFolderPicker() {} // no folder picker on web today
void Platform::drainPickers() {}     // web pickers dispatch inline

} // namespace nodehammer::viewer::platform

extern "C" {

// JS-picker batch lifecycle. Bytes flow into the App's long-lived project
// directly; the begin/end pair no longer carries state. The handle is a
// fixed non-null sentinel kept only so the JS-side ABI doesn't change
// (the shim still threads it through, but the C side ignores it).
namespace {
char nh_picker_handle_sentinel = 0;
} // namespace

EMSCRIPTEN_KEEPALIVE
void *nh_viewer_begin_upload_batch() { return static_cast<void *>(&nh_picker_handle_sentinel); }

EMSCRIPTEN_KEEPALIVE
void nh_viewer_add_upload(void * /*handle*/, const char *filename, const std::uint8_t *data,
                          std::size_t size) {
    std::println(stderr, "[viewer] nh_viewer_add_upload: filename='{}' size={}",
                 filename != nullptr ? filename : "(null)", size);
    if (filename == nullptr) {
        return;
    }
    auto *app = nodehammer::viewer::App::instance();
    if (app == nullptr) {
        std::println(stderr, "[viewer] nh_viewer_add_upload: App::instance() is null");
        return;
    }
    auto *project = app->project();
    if (project == nullptr) {
        std::println(stderr, "[viewer] nh_viewer_add_upload: app->project() is null");
        return;
    }
    project->addBytes(filename, std::as_bytes(std::span{data, size}));
}

EMSCRIPTEN_KEEPALIVE
void nh_viewer_end_upload_batch(void * /*handle*/) {
    // No-op: each addBytes already updated the project; the build trigger
    // picks up the new state on the next frame poll.
}

} // extern "C"

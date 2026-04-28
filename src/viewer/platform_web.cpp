#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/drop_asset_source.hpp>
#include <nodehammer/viewer/platform.hpp>

#include <emscripten/emscripten.h>
#include <sokol_app.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <utility>
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

void commitUrlState(const std::string &state_query, const std::string &managed_keys) {
    nh_viewer_commit_url_state(state_query.c_str(), managed_keys.c_str());
}

void dispatchWebFilePicker() { nh_viewer_open_file_picker(); }

void runNativeFilePicker(const NativeFilePathHandler & /*handler*/) {
    // NFD doesn't exist on web; the web picker is dispatched via
    // dispatchWebFilePicker and bytes arrive through the C upload export.
}

namespace {

// Refcounted batch shared across the N in-flight fetches a single drop
// kicks off. The batch owns the freshly-allocated source while bytes are
// arriving; only the last completing fetch installs it on the App. The
// shared_ptr lives in each per-file ctx, so when the last ctx destructs
// the batch frees itself naturally.
struct WebDropBatch {
    std::unique_ptr<DropAssetSource> source;
    int pending{0};
    App *app{nullptr};
};

// Per-file fetch context. sokol writes the file's bytes into `buffer` then
// invokes the callback; we transfer ownership of the ctx via `release()`
// into req.user_data and reclaim with a fresh unique_ptr in the callback.
struct WebDropCtx {
    std::vector<std::byte> buffer;
    std::string filename;
    std::shared_ptr<WebDropBatch> batch;
};

void webDropFetchCallback(const sapp_html5_fetch_response *response) {
    std::unique_ptr<WebDropCtx> ctx{static_cast<WebDropCtx *>(response->user_data)};
    if (response->succeeded) {
        ctx->batch->source->addBytes(
            ctx->filename, std::span<const std::byte>{
                               ctx->buffer.data(), static_cast<std::size_t>(response->data.size)});
    } else {
        std::println(stderr, "viewer: failed to fetch dropped file '{}'", ctx->filename);
    }
    if (--ctx->batch->pending == 0 && ctx->batch->app != nullptr) {
        ctx->batch->app->setSource(std::move(ctx->batch->source));
    }
}

} // namespace

void dispatchDroppedFiles(App &app) {
    const int n = sapp_get_num_dropped_files();
    if (n == 0) {
        return;
    }
    auto batch = std::make_shared<WebDropBatch>();
    batch->source = std::make_unique<DropAssetSource>();
    batch->pending = n;
    batch->app = &app;
    for (int i = 0; i < n; ++i) {
        const auto size = sapp_html5_get_dropped_file_size(i);
        auto ctx = std::make_unique<WebDropCtx>();
        ctx->filename = sapp_get_dropped_file_path(i);
        ctx->buffer.resize(static_cast<std::size_t>(size));
        ctx->batch = batch;
        sapp_html5_fetch_request req{};
        req.dropped_file_index = i;
        req.callback = &webDropFetchCallback;
        req.buffer = sapp_range{ctx->buffer.data(), ctx->buffer.size()};
        // Ownership transfers to the callback via user_data; callback
        // reclaims with a unique_ptr on the receiving end.
        req.user_data = ctx.release();
        sapp_html5_fetch_dropped_file(&req);
    }
}

} // namespace nodehammer::viewer::platform

extern "C" {

// JS-picker batch lifecycle. The JS shim holds the returned handle in a
// closure-local variable, threads it through `add` calls, and passes it to
// `end`. No file-scope state on the C side; the batch lives on the heap
// for exactly the duration of the gesture.

EMSCRIPTEN_KEEPALIVE
void *nh_viewer_begin_upload_batch() { return new nodehammer::viewer::DropAssetSource; }

EMSCRIPTEN_KEEPALIVE
void nh_viewer_add_upload(void *handle, const char *filename, const std::uint8_t *data,
                          std::size_t size) {
    if (handle == nullptr || filename == nullptr) {
        return;
    }
    auto *source = static_cast<nodehammer::viewer::DropAssetSource *>(handle);
    source->addBytes(filename, std::as_bytes(std::span{data, size}));
}

EMSCRIPTEN_KEEPALIVE
void nh_viewer_end_upload_batch(void *handle) {
    if (handle == nullptr) {
        return;
    }
    std::unique_ptr<nodehammer::viewer::DropAssetSource> source{
        static_cast<nodehammer::viewer::DropAssetSource *>(handle)};
    if (auto *app = nodehammer::viewer::App::instance()) {
        app->setSource(std::move(source));
    }
}

} // extern "C"

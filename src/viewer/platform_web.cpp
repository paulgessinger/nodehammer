#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/asset_source.hpp>
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

// Per-fetch context for sokol's async dropped-file byte fetch on web. sokol
// writes the file's bytes into `buffer` and then invokes the callback once
// the FileReader resolves; we keep the buffer alive in the meantime by
// transferring ownership to the request via `release()` and reclaiming it
// in the callback through a fresh unique_ptr.
struct WebDropCtx {
    std::vector<std::byte> buffer;
    std::string filename;
    AssetSource *source{nullptr};
};

void webDropFetchCallback(const sapp_html5_fetch_response *response) {
    std::unique_ptr<WebDropCtx> ctx{static_cast<WebDropCtx *>(response->user_data)};
    if (response->succeeded) {
        ctx->source->ingestBytes(
            ctx->filename, std::span<const std::byte>{
                               ctx->buffer.data(), static_cast<std::size_t>(response->data.size)});
    } else {
        std::println(stderr, "viewer: failed to fetch dropped file '{}'", ctx->filename);
    }
}

} // namespace

void dispatchDroppedFiles(AssetSource &source) {
    const int n = sapp_get_num_dropped_files();
    for (int i = 0; i < n; ++i) {
        const auto size = sapp_html5_get_dropped_file_size(i);
        auto ctx = std::make_unique<WebDropCtx>();
        ctx->filename = sapp_get_dropped_file_path(i);
        ctx->buffer.resize(static_cast<std::size_t>(size));
        ctx->source = &source;
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

EMSCRIPTEN_KEEPALIVE
void nh_viewer_deliver_upload(const char *filename, const std::uint8_t *data, std::size_t size) {
    if (filename == nullptr) {
        return;
    }
    auto *app = nodehammer::viewer::App::instance();
    if (app == nullptr) {
        return;
    }
    auto *source = app->source();
    if (source == nullptr) {
        return;
    }
    source->ingestBytes(filename, std::as_bytes(std::span{data, size}));
}

} // extern "C"

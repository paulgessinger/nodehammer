#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/platform.hpp>

#include <emscripten/emscripten.h>

#include <cstddef>
#include <cstdint>

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

} // namespace nodehammer::viewer::platform

extern "C" {

EMSCRIPTEN_KEEPALIVE
void nh_viewer_deliver_upload(const char *filename, const std::uint8_t *data, std::size_t size) {
    if (filename == nullptr) {
        return;
    }
    if (auto *app = nodehammer::viewer::App::instance()) {
        app->deliverUpload(filename, data, size);
    }
}

} // extern "C"

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

// clang-format off
EM_JS(void, nh_viewer_install_window_observers, (), {
    if (Module.__nhWindowObserversInstalled) {
        return;
    }
    Module.__nhWindowObserversInstalled = true;

    var canvas = Module.canvas || document.getElementById('canvas');
    if (!canvas) {
        return;
    }

    var dragDepth = 0;
    function fileLike(ev) {
        var types = ev.dataTransfer && ev.dataTransfer.types;
        if (!types) return false;
        for (var i = 0; i < types.length; ++i) {
            if (types[i] === 'Files') return true;
        }
        return false;
    }
    function fileCount(ev) {
        var items = ev.dataTransfer && ev.dataTransfer.items;
        return items ? items.length : 0;
    }
    function setDrag(active, ev) {
        Module._nh_viewer_platform_set_drag_hover(
            active ? 1 : 0,
            ev ? ev.clientX : 0,
            ev ? ev.clientY : 0,
            ev ? fileCount(ev) : 0,
            ev && fileLike(ev) ? 1 : 0);
    }
    function onDragEnter(ev) {
        ++dragDepth;
        setDrag(true, ev);
    }
    function onDragOver(ev) {
        if (fileLike(ev)) {
            ev.preventDefault();
        }
        setDrag(true, ev);
    }
    function onDragLeave(ev) {
        dragDepth = Math.max(0, dragDepth - 1);
        if (dragDepth === 0) {
            setDrag(false, ev);
        }
    }
    function onDrop(ev) {
        dragDepth = 0;
        setDrag(false, ev);
    }
    canvas.addEventListener('dragenter', onDragEnter);
    canvas.addEventListener('dragover', onDragOver);
    canvas.addEventListener('dragleave', onDragLeave);
    canvas.addEventListener('drop', onDrop);
    window.addEventListener('blur', function(ev) {
        dragDepth = 0;
        setDrag(false, ev);
    });

    var gestureScale = 1.0;
    function pushPinch(type, scaleDelta, ev) {
        Module._nh_viewer_platform_push_pinch(
            type, scaleDelta,
            ev ? ev.clientX : 0,
            ev ? ev.clientY : 0,
            ev ? ((ev.shiftKey ? 1 : 0) | (ev.ctrlKey ? 2 : 0) | (ev.altKey ? 4 : 0) | (ev.metaKey ? 8 : 0)) : 0);
    }
    canvas.addEventListener('wheel', function(ev) {
        if (!(ev.ctrlKey || ev.metaKey)) {
            return;
        }
        ev.preventDefault();
        pushPinch(1, Math.exp(-ev.deltaY * 0.01), ev);
    }, { passive: false });
    canvas.addEventListener('gesturestart', function(ev) {
        gestureScale = ev.scale || 1.0;
        ev.preventDefault();
        pushPinch(0, 1.0, ev);
    }, { passive: false });
    canvas.addEventListener('gesturechange', function(ev) {
        var next = ev.scale || gestureScale;
        var delta = gestureScale !== 0 ? next / gestureScale : 1.0;
        gestureScale = next;
        ev.preventDefault();
        pushPinch(1, delta, ev);
    }, { passive: false });
    canvas.addEventListener('gestureend', function(ev) {
        ev.preventDefault();
        pushPinch(2, 1.0, ev);
    }, { passive: false });

    var touchDistance = 0;
    function twoTouchDistance(touches) {
        if (!touches || touches.length < 2) return 0;
        var dx = touches[0].clientX - touches[1].clientX;
        var dy = touches[0].clientY - touches[1].clientY;
        return Math.sqrt(dx * dx + dy * dy);
    }
    function touchCenter(touches) {
        return {
            clientX: (touches[0].clientX + touches[1].clientX) * 0.5,
            clientY: (touches[0].clientY + touches[1].clientY) * 0.5,
            ctrlKey: false,
            shiftKey: false,
            altKey: false,
            metaKey: false
        };
    }
    canvas.addEventListener('touchstart', function(ev) {
        if (ev.touches.length === 2) {
            touchDistance = twoTouchDistance(ev.touches);
            ev.preventDefault();
            pushPinch(0, 1.0, touchCenter(ev.touches));
        }
    }, { passive: false });
    canvas.addEventListener('touchmove', function(ev) {
        if (ev.touches.length === 2 && touchDistance > 0) {
            var next = twoTouchDistance(ev.touches);
            ev.preventDefault();
            pushPinch(1, next / touchDistance, touchCenter(ev.touches));
            touchDistance = next;
        }
    }, { passive: false });
    canvas.addEventListener('touchend', function(ev) {
        if (touchDistance > 0 && ev.touches.length < 2) {
            touchDistance = 0;
            pushPinch(2, 1.0, ev.changedTouches && ev.changedTouches[0] ? ev.changedTouches[0] : ev);
        }
    }, { passive: false });
    canvas.addEventListener('touchcancel', function(ev) {
        if (touchDistance > 0) {
            touchDistance = 0;
            pushPinch(3, 1.0, ev.changedTouches && ev.changedTouches[0] ? ev.changedTouches[0] : ev);
        }
    }, { passive: false });
});
// clang-format on

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
    WindowCustomizationRequest window_request;
    PlatformWindowState window_state;
    std::vector<PlatformGestureEvent> gesture_events;
};

namespace {
Platform::Impl *g_impl{nullptr};
} // namespace

Platform::Platform(App &app) : impl_(std::make_unique<Impl>(app)) { g_impl = impl_.get(); }
Platform::~Platform() {
    if (g_impl == impl_.get()) {
        g_impl = nullptr;
    }
}

void Platform::configureWindowDesc(sapp_desc & /*desc*/, const Config & /*cfg*/,
                                   const WindowCustomizationRequest &request) {
    impl_->window_request = request;
}

void Platform::attachWindow(const WindowCustomizationRequest &request) {
    impl_->window_request = request;
    impl_->window_state.drag_hover.supported = request.track_drag_hover;
    impl_->window_state.supports_pinch_gesture = request.track_platform_gestures;
    nh_viewer_install_window_observers();
}

void Platform::handleWindowEvent(const sapp_event *ev) {
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        impl_->window_state.drag_hover.active = false;
    }
}

void Platform::beginFrameWindowSync() {}

const PlatformWindowState &Platform::windowState() const noexcept { return impl_->window_state; }

std::vector<PlatformGestureEvent> Platform::takeGestureEvents() {
    return std::exchange(impl_->gesture_events, {});
}

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

void setWebDragHover(bool active, float x, float y, int file_count, bool file_like) {
    if (g_impl == nullptr) {
        return;
    }
    DragHoverState state{};
    state.supported = true;
    state.active = active;
    state.file_like = file_like;
    state.file_count = active ? file_count : 0;
    state.x = x;
    state.y = y;
    g_impl->window_state.drag_hover = state;
}

void pushWebPinch(int type, float scale_delta, float x, float y, uint32_t modifiers) {
    if (g_impl == nullptr) {
        return;
    }
    PlatformGestureEvent event{};
    switch (type) {
    case 0:
        event.type = GestureType::PinchBegin;
        break;
    case 2:
        event.type = GestureType::PinchEnd;
        break;
    case 3:
        event.type = GestureType::PinchCancel;
        break;
    case 1:
    default:
        event.type = GestureType::PinchUpdate;
        break;
    }
    event.scale_delta = scale_delta > 0.f ? scale_delta : 1.f;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    g_impl->gesture_events.push_back(event);
}

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

EMSCRIPTEN_KEEPALIVE
void nh_viewer_platform_set_drag_hover(int active, float x, float y, int file_count,
                                       int file_like) {
    nodehammer::viewer::platform::setWebDragHover(active != 0, x, y, file_count, file_like != 0);
}

EMSCRIPTEN_KEEPALIVE
void nh_viewer_platform_push_pinch(int type, float scale_delta, float x, float y,
                                   std::uint32_t modifiers) {
    nodehammer::viewer::platform::pushWebPinch(type, scale_delta, x, y, modifiers);
}

} // extern "C"

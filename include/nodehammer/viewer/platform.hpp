#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace nodehammer::viewer {
class App;
} // namespace nodehammer::viewer

namespace nodehammer::viewer::platform {

#ifdef __EMSCRIPTEN__
inline constexpr bool kIsWeb = true;
#else
inline constexpr bool kIsWeb = false;
#endif

/// Push the current sokol_app FILES_DROPPED batch into `app`'s long-lived
/// project. Multi-gesture accumulation is the norm: the previous drop's
/// files stay in the bag, and this gesture's files get added on top. On
/// native this is fully synchronous. On web each file's bytes arrive via
/// an async sokol_html5 fetch; the per-file callback calls
/// `app.project()->addBytes(...)` independently, so a partial gesture
/// already advances the build trigger.
void dispatchDroppedFiles(App &app);

/// Push the viewer's persisted state into the browser URL's query string,
/// replacing any keys named in `managed_keys` (comma-separated). No-op on
/// native — the URL has no meaning for a windowed app.
void commitUrlState(const std::string &state_query, const std::string &managed_keys);

/// Open the browser's transient `<input type=file multiple>` picker
/// inline. The browser requires `input.click()` to run from the user-
/// gesture stack, so this dispatches synchronously; selected files arrive
/// later via the `nh_viewer_*_upload_batch` C exports, which push bytes
/// into the App's existing project. No-op on native.
void dispatchWebFilePicker();

/// Run the native (NFD) file picker modally and invoke `handler` for each
/// selected path. The modal enters a nested run loop on macOS / Windows,
/// so callers must invoke this only after the current ImGui frame has
/// fully rendered. No-op on web.
using NativeFilePathHandler = std::function<void(const std::filesystem::path &)>;
void runNativeFilePicker(const NativeFilePathHandler &handler);

} // namespace nodehammer::viewer::platform

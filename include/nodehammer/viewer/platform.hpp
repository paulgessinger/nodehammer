#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace nodehammer::viewer {
class AssetSource;
} // namespace nodehammer::viewer

namespace nodehammer::viewer::platform {

#ifdef __EMSCRIPTEN__
inline constexpr bool kIsWeb = true;
#else
inline constexpr bool kIsWeb = false;
#endif

/// Route the current sokol_app FILES_DROPPED batch into `source`. On native
/// each dropped file is a real filesystem path (handed to
/// `AssetSource::ingestLocalFile` synchronously). On web the bytes have to
/// be fetched asynchronously from the browser; the platform impl owns each
/// fetch's lifetime and calls `AssetSource::ingestBytes` once the bytes
/// arrive. Caller is responsible for ensuring `source` outlives any
/// outstanding async fetches.
void dispatchDroppedFiles(AssetSource &source);

/// Push the viewer's persisted state into the browser URL's query string,
/// replacing any keys named in `managed_keys` (comma-separated). No-op on
/// native — the URL has no meaning for a windowed app.
void commitUrlState(const std::string &state_query, const std::string &managed_keys);

/// Open the browser's transient `<input type=file multiple>` picker
/// inline. The browser requires `input.click()` to run from the user-
/// gesture stack, so this dispatches synchronously; selected files arrive
/// later via the C upload export, which routes them to
/// `App::instance()->deliverUpload`. No-op on native.
void dispatchWebFilePicker();

/// Run the native (NFD) file picker modally and invoke `handler` for each
/// selected path. The modal enters a nested run loop on macOS / Windows,
/// so callers must invoke this only after the current ImGui frame has
/// fully rendered. No-op on web.
using NativeFilePathHandler = std::function<void(const std::filesystem::path &)>;
void runNativeFilePicker(const NativeFilePathHandler &handler);

} // namespace nodehammer::viewer::platform

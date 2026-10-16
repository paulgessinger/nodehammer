#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct sapp_desc;
struct sapp_event;

namespace nodehammer::viewer {
class App;
class ProjectFs;
struct Config;
} // namespace nodehammer::viewer

namespace nodehammer::viewer::platform {

#ifdef __EMSCRIPTEN__
inline constexpr bool kIsWeb = true;
#else
inline constexpr bool kIsWeb = false;
#endif

/// Construct the App's "empty project" backend. Native returns a
/// write-through `NativeBagProjectFs` rooted at a process-owned tmp
/// dir (strategy doc step 3); web returns an empty `ArchiveProjectFs`
/// working set (provenance `Empty`, §0 reshape / R1) that loose drops
/// accumulate into and application mode persists to IDB. Defined
/// per-platform so the App TU stays free of backend headers.
std::unique_ptr<ProjectFs> makeEmptyBag();

struct WindowCustomizationRequest {
    std::string persistence_id{"nodehammer.viewer.window"};
    bool restore_placement{true};
    bool hide_titlebar_chrome{false};
    bool track_drag_hover{true};
    bool track_platform_gestures{true};
};

struct WindowChromeState {
    bool titlebar_transparent{false};
    bool traffic_lights_overlap_content{false};
    float content_top_inset{0.f};
    float content_left_inset{0.f};
};

struct DragHoverState {
    bool supported{false};
    bool active{false};
    bool file_like{false};
    int file_count{0};
    float x{0.f};
    float y{0.f};
};

struct PlatformWindowState {
    WindowChromeState chrome;
    DragHoverState drag_hover;
    bool supports_window_restoration{false};
    bool supports_hidden_titlebar{false};
    bool supports_pinch_gesture{false};
};

enum class GestureType { PinchBegin, PinchUpdate, PinchEnd, PinchCancel };

struct PlatformGestureEvent {
    GestureType type{GestureType::PinchUpdate};
    float scale_delta{1.f};
    float x{0.f};
    float y{0.f};
    uint32_t modifiers{0};
};

/// Platform surface owned by the App. Concrete pImpl: `struct Impl` is
/// forward-declared here and defined differently in `platform_native.cpp`
/// vs `platform_web.cpp` — exactly one TU is linked per executable, so
/// runtime polymorphism would be wasted overhead. State that previously
/// lived in file-static globals (picker latches) lives on `Impl` as
/// members, scoped to the owning App's lifetime.
///
/// The destructor is defined out-of-line in each platform TU so the
/// compiler can see `Impl`'s full type when destroying the unique_ptr.
class Platform {
  public:
    struct Impl;

    /// Construct with a back-pointer to the owning App. App is non-
    /// movable, lives in a static slot, and strictly outlives its
    /// Platform, so the bare reference stored in `Impl` is safe across
    /// the App's lifetime.
    explicit Platform(App &app);
    ~Platform();
    Platform(const Platform &) = delete;
    Platform &operator=(const Platform &) = delete;

    /// Give the platform one last chance to adjust the sokol descriptor
    /// before the OS window is created.
    void configureWindowDesc(sapp_desc &desc, const Config &cfg,
                             const WindowCustomizationRequest &request);

    /// Attach platform-native window customisations after sokol has
    /// created the OS window.
    void attachWindow(const WindowCustomizationRequest &request);

    /// Let platform-specific code observe raw sokol events before the App
    /// handles its portable state transitions.
    void handleWindowEvent(const sapp_event *ev);

    /// Poll/synchronise any native state that is easiest to observe once
    /// per frame.
    void beginFrameWindowSync();

    [[nodiscard]] const PlatformWindowState &windowState() const noexcept;
    [[nodiscard]] std::vector<PlatformGestureEvent> takeGestureEvents();

    /// Whether any platform gesture events are queued but not yet drained
    /// by `takeGestureEvents`. Read-only peek used by the idle frame-rate
    /// gate: trackpad pinch-zoom arrives through this queue rather than as
    /// sokol input events, so without this it wouldn't keep the viewer at
    /// full refresh while the user is gesturing.
    [[nodiscard]] bool hasPendingGestures() const noexcept;

    /// Push the current sokol_app FILES_DROPPED batch into the App's
    /// long-lived project. Multi-gesture accumulation is the norm: the
    /// previous drop's files stay in the bag, and this gesture's files
    /// get added on top. Native is fully synchronous; web fires per-
    /// file `sokol_html5` fetches and the byte callbacks update the
    /// project independently, so a partial gesture already advances
    /// the build trigger.
    void dispatchDroppedFiles();

    /// Load/save small persistent text blobs owned by the viewer. Native
    /// stores these under the platform config directory; web stores them in
    /// browser localStorage. Intended for TOML settings and Dear ImGui ini
    /// text, not bulk binary caches.
    [[nodiscard]] std::optional<std::string> loadPersistentText(const std::string &key) const;
    void savePersistentText(const std::string &key, const std::string &bytes);

    /// Push the viewer's persisted state into the browser URL's query
    /// string, replacing any keys named in `managed_keys` (comma-
    /// separated). No-op on native — the URL has no meaning for a
    /// windowed app.
    /// @TODO: Evolve this to a uniform UI state persistence model
    void commitUrlState(const std::string &state_query, const std::string &managed_keys);

    /// Open an external URL. Web opens a new tab/window; native is
    /// currently a no-op.
    void openUrl(const std::string &url);

    /// Deliver an exported image (e.g. a screenshot PNG) to the user. Native
    /// writes it into the process's current working directory and returns the
    /// path written; web triggers a browser download and returns the suggested
    /// filename. Returns nullopt on failure. `bytes` is consumed during the
    /// call — safe to free afterwards.
    std::optional<std::string> saveExportedImage(const std::string &filename,
                                                 std::span<const std::byte> bytes);

    /// Deliver a `.nhproj` archive blob to the user. Web triggers a browser
    /// download under `filename` (how unbound web archives persist). Native is a
    /// no-op — native archives are written to a path picked via
    /// `saveArchivePicker()`. `bytes` is consumed during the call.
    void downloadArchive(const std::string &filename, std::span<const std::byte> bytes);

    /// Web application-mode project persistence to IndexedDB (the working set that
    /// makes the web app feel native — it survives reload). Native is a no-op:
    /// native modes persist through their own on-disk backing.
    ///   - `loadProjectBlob()` kicks an async IDB read; when it resolves the
    ///     runtime calls `App::onProjectBlobLoaded(bytes)` (empty span on a miss).
    ///   - `saveProjectBlob(bytes)` writes the blob (fire-and-forget).
    ///   - `clearProjectBlob()` deletes it (Close project).
    void loadProjectBlob();
    void saveProjectBlob(std::span<const std::byte> bytes);
    void clearProjectBlob();

    /// Web "Publish package": fetch the running app's own same-origin runtime
    /// siblings (index.html + the gles3/wgpu/compute js+wasm) and hand each back
    /// to `App::addPackageFile`, then call `App::finalizePackage` to serialize the
    /// self-contained deployable and download it. Native is a no-op for now — a
    /// native publish would copy a staged web runtime (§6.6, feasibility TBD).
    void fetchRuntimeForPublish();

    /// Request a "pick files" gesture. Web dispatches the browser's
    /// transient `<input type=file multiple>` inline (the browser
    /// requires `input.click()` to run from the user-gesture stack);
    /// native records a latch and runs the NFD modal in
    /// `drainPickers()` to avoid re-entering the active ImGui frame
    /// (NFD's modal spawns a nested run loop on macOS / Windows).
    void openFilePicker();

    /// Request a "pick a folder" gesture. Native records a latch
    /// drained by `drainPickers()`; web is a no-op (no folder picker
    /// without `<input webkitdirectory>`, Stage 4+ concern). The App
    /// should keep the "Open folder…" button hidden on web — `kIsWeb`
    /// is the gate.
    void openFolderPicker();

    /// Request an "open archive" gesture. Native records a latch drained by
    /// `drainPickers()` that opens the picked `.zip` as an `ArchiveProjectFs`
    /// (strategy doc step 6); web is a no-op — there is no live archive mode on
    /// web, so the "Open archive…" menu item is hidden behind `kIsWeb`.
    void openArchivePicker();

    /// Request a "save archive as…" gesture for the active (unbound) archive.
    /// Native records a latch drained by `drainPickers()` that runs an NFD save
    /// dialog and, on confirm, calls `App::saveActiveArchiveTo(path)` (binding the
    /// archive to the chosen path). Web is a no-op — web archives persist via
    /// `downloadArchive`.
    void saveArchivePicker();

    /// Run any pending picker modal latched by `openFilePicker` /
    /// `openFolderPicker` / `openArchivePicker` / `saveArchivePicker`. Called once at end of frame
    /// after the ImGui
    /// pass has been rendered and committed. No-op on web — web
    /// pickers dispatch inline at click time.
    void drainPickers();

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer::platform

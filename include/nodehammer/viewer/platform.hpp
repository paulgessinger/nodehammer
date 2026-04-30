#pragma once

#include <memory>
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
    /// Construct with a back-pointer to the owning App. App is non-
    /// movable, lives in a static slot, and strictly outlives its
    /// Platform, so the bare reference stored in `Impl` is safe across
    /// the App's lifetime.
    explicit Platform(App &app);
    ~Platform();
    Platform(const Platform &) = delete;
    Platform &operator=(const Platform &) = delete;

    /// Push the current sokol_app FILES_DROPPED batch into the App's
    /// long-lived project. Multi-gesture accumulation is the norm: the
    /// previous drop's files stay in the bag, and this gesture's files
    /// get added on top. Native is fully synchronous; web fires per-
    /// file `sokol_html5` fetches and the byte callbacks update the
    /// project independently, so a partial gesture already advances
    /// the build trigger.
    void dispatchDroppedFiles();

    /// Push the viewer's persisted state into the browser URL's query
    /// string, replacing any keys named in `managed_keys` (comma-
    /// separated). No-op on native — the URL has no meaning for a
    /// windowed app.
    /// @TODO: Evolve this to a uniform UI state persistence model
    void commitUrlState(const std::string &state_query, const std::string &managed_keys);

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

    /// Run any pending picker modal latched by `openFilePicker` /
    /// `openFolderPicker`. Called once at end of frame after the ImGui
    /// pass has been rendered and committed. No-op on web — web
    /// pickers dispatch inline at click time.
    void drainPickers();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer::platform

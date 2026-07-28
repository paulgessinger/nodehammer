#pragma once

#include <nodehammer/viewer/config.hpp>
#include <nodehammer/viewer/png_export.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace nodehammer {
namespace detail {
struct RenderScene;
} // namespace detail
} // namespace nodehammer

namespace nodehammer::viewer {

class ProjectFs;

/// Top-level viewer lifecycle. Owns the sokol_app window/event loop, the
/// sokol_gfx render context, and the ImGui state. Native and emscripten
/// builds share the same App; the only divergence is in run().
///
/// Effectively a singleton — sokol_app's `sapp_run` only supports one
/// window per process, the IBL bake state machine assumes one, etc.
/// Construction is gated through `App::Handle`, which both enforces the
/// singleton constraint at runtime and owns the platform-specific
/// teardown contract. `App::instance()` is the canonical way for platform
/// glue (the web upload C export, the native picker modal) to reach the
/// live instance without threading a pointer through static callbacks.
class App {
  private:
    // Passkey tag for the public constructor. Declared private so external
    // callers can't name it to invoke the ctor. `App::Handle` (also
    // nested in App) sees it normally and can construct one to forward
    // through `std::make_unique<App>` without needing to friend the
    // allocator.
    struct PrivateTag {};

  public:
    /// RAII handle for the singleton App. Constructing one creates the
    /// App; destroying it tears down on native, but is a no-op on
    /// emscripten — `sapp_run` returns immediately on web after registering
    /// the main loop, so the App must outlive the calling scope or
    /// subsequent frame callbacks would dispatch against freed memory. On
    /// emscripten the App stays alive in a function-local-static
    /// `unique_ptr` and is destroyed at program exit.
    class Handle {
      public:
        explicit Handle(Config cfg);
        ~Handle();
        Handle(const Handle &) = delete;
        Handle &operator=(const Handle &) = delete;
        Handle(Handle &&) = delete;
        Handle &operator=(Handle &&) = delete;

        [[nodiscard]] App *operator->() const noexcept;
        [[nodiscard]] App &operator*() const noexcept;
    };

    ~App();

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    /// Hand the viewer a tessellated scene to render. The App takes a shared
    /// reference; safe to drop the local copy afterwards. Pass nullptr to
    /// clear (revert to demo geometry). May be called before or after run();
    /// scene_renderer uploads lazily on the next frame.
    void setScene(std::shared_ptr<const detail::RenderScene> scene);

    /// Hand the viewer a project. Each frame the App polls it; while
    /// Fetching it draws a progress / placeholder panel, and on Ready it
    /// builds the scene from the project's resolved paths. The project is
    /// long-lived: drag-drop and the file-picker push files into the
    /// existing project rather than allocate a new one. Replacing the
    /// project (e.g. swapping the empty working set for an opened archive, or
    /// wrapping the current one in an overlay/watcher decorator) clears the
    /// current scene. Honors a self-describing archive's project manifest.
    void setProject(std::unique_ptr<ProjectFs> project);

    /// Live project the App is polling, never null after construction.
    /// Platform glue (drop callbacks, JS picker C exports) calls this each
    /// time it has files to push. Do NOT cache the returned pointer across
    /// frames — future stages will allow transparent decoration via
    /// `setProject(make_unique<Wrapper>(std::move(...)))`, and a cached
    /// pointer would bypass the wrapper.
    [[nodiscard]] ProjectFs *project() const noexcept;

    /// Add files to the current project through the App. The ProjectFs decides
    /// whether to accept, reject, or require confirmation; App owns the UI.
    /// A dropped/picked `.nhproj` is recognised and opened as a whole project
    /// (see `openArchiveFromBytes`) rather than added as a file.
    void addProjectPath(const std::filesystem::path &path);
    void addProjectBytes(const std::string &filename, std::span<const std::byte> bytes);

    /// Open `.nhproj` bytes as a live project (unbound `ArchiveProjectFs`,
    /// provenance `Local`). Used by the web open-archive picker / drop and any
    /// caller that already has the archive bytes in hand. On a bad archive it
    /// surfaces an error notification and leaves the current project in place.
    void openArchiveFromBytes(std::span<const std::byte> bytes);

    /// Open fetched `.nhproj` bytes as a web *viewer-mode* project (provenance
    /// `Remote`): content-`locked` per the sidecar, never persisted to IDB,
    /// re-fetched from source on reload. Called by the startup archive fetch.
    void openArchiveRemote(std::span<const std::byte> bytes, bool locked);

    /// Publish the current project as a self-contained web package (§6.6). Web
    /// only: seeds a package with the sidecar + `.nhproj`, then the platform fetches
    /// the app's own runtime siblings — `addPackageFile` collects each and
    /// `finalizePackage` serializes + downloads `nodehammer-package.zip`. The two
    /// callbacks are invoked by the runtime; `publishPackage` is the entry point.
    void publishPackage();
    void addPackageFile(const std::string &name, std::span<const std::byte> bytes);
    void finalizePackage();

    /// Web application-mode IDB persistence hooks (no-ops on native / viewer mode):
    ///   - `restoreWebProjectFromIdb()` kicks the async cold-load at startup.
    ///   - `onProjectBlobLoaded(bytes)` is the runtime callback for that load;
    ///     it restores the saved working set if the project is still pristine.
    ///   - `flushWebProjectPersistence()` force-writes the working set to IDB
    ///     (called on `beforeunload`).
    void restoreWebProjectFromIdb();
    void onProjectBlobLoaded(std::span<const std::byte> bytes);
    void flushWebProjectPersistence();

    /// Promote the current project into an in-memory archive bundle seeded with
    /// its archivable content (build closure for filesystem mode, whole working
    /// set for bounded backends), re-seeding the current root keys so the scene
    /// stays live. Switches into a live, unbound archive mode the user can curate
    /// (drop files) and then Save. Cross-platform (strategy doc step 7).
    void createArchiveFromScene();

#ifndef __EMSCRIPTEN__
    /// Serialize the active archive to `path`, binding it there (native save-as).
    /// Called by the save-archive picker once the user has chosen a path.
    void saveActiveArchiveTo(const std::filesystem::path &path);
#endif

    /// Flush viewer state immediately. Normal frame/end-of-app saves call this
    /// internally; platform code uses it for quit/page-unload paths where the
    /// next frame or regular cleanup may not run.
    void savePersistentState();

    /// Tell the App which project keys are the build's root inputs:
    /// the TOML config and the FlatBuffer geometry file. Used by web
    /// URL mode (the JS layer hands over explicit keys from
    /// `?config=…&input=…`); bag mode discovers them via extension-
    /// based recognition over the project's progress entries. Either
    /// argument may be empty to clear the corresponding root.
    void setRootKeys(std::string config_key, std::string geometry_key);

    /// Request a one-shot headless screenshot: once the scene has loaded and
    /// settled, render a PNG to `path` (with all quality maxed, per
    /// `settings`) and then quit. Intended for CLI / automated rendering; call
    /// before run(). Reuses the interactive export pipeline.
    void requestScreenshot(std::string path, PngExportSettings settings);

    /// Request headless benchmark mode: once the scene settles, drive a fixed
    /// camera/state sequence (cut-on hold/orbit/zoom, then cut-off), measure
    /// per-pass GPU time over each window, grab a screenshot per segment, write
    /// the results JSON to `json_out_path` (and stdout), then quit. `scene_label`
    /// is echoed into the JSON. Call before run(). D3D11 is the only backend with
    /// real GPU timings; elsewhere the timing fields are marked invalid.
    void requestBench(std::string json_out_path, std::string scene_label,
                      float render_scale = 1.0f);

    /// Native: blocks until the window closes; returns the exit code.
    /// Emscripten: registers the main loop with the runtime and returns 0
    /// while the loop continues to fire from the event queue.
    int run();

    /// The live App, or nullptr if no `App::Handle` is currently
    /// constructed. Safe to call from any thread that is also live during
    /// the App's lifetime.
    [[nodiscard]] static App *instance();

    // Public so the file-scope upload helpers in app.cpp (sokol's drop
    // fetch callback) can access App::Impl. Definition lives entirely in
    // the .cpp; nothing else can manipulate it from this header.
    struct Impl;

    App(PrivateTag, Config cfg);

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

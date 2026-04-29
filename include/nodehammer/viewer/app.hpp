#pragma once

#include <nodehammer/viewer/config.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace nodehammer {
struct RenderScene;
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
    void setScene(std::shared_ptr<const RenderScene> scene);

    /// Hand the viewer a project. Each frame the App polls it; while
    /// Fetching it draws a progress / placeholder panel, and on Ready it
    /// builds the scene from the project's resolved paths. The project is
    /// long-lived: drag-drop and the file-picker push files into the
    /// existing project rather than allocate a new one. Replacing the
    /// project (e.g. swapping a BagProjectFs for a UrlProjectFs at startup,
    /// or wrapping the current one in a future overlay/watcher decorator)
    /// clears the current scene.
    void setProject(std::unique_ptr<ProjectFs> project);

    /// Live project the App is polling, never null after construction.
    /// Platform glue (drop callbacks, JS picker C exports) calls this each
    /// time it has files to push. Do NOT cache the returned pointer across
    /// frames — future stages will allow transparent decoration via
    /// `setProject(make_unique<Wrapper>(std::move(...)))`, and a cached
    /// pointer would bypass the wrapper.
    [[nodiscard]] ProjectFs *project() const noexcept;

    /// Tell the App which project keys are the build's root inputs:
    /// the TOML config and the FlatBuffer geometry file. Used by web
    /// URL mode (the JS layer hands over explicit keys from
    /// `?config=…&input=…`); bag mode discovers them via extension-
    /// based recognition over the project's progress entries. Either
    /// argument may be empty to clear the corresponding root.
    void setRootKeys(std::string config_key, std::string geometry_key);

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

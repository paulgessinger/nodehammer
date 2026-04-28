#pragma once

#include <nodehammer/viewer/config.hpp>

#include <cstddef>
#include <memory>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

class AssetSource;

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

    /// Hand the viewer an asset source. Each frame the App polls it; while
    /// Fetching it draws a progress / placeholder panel, and on Ready it
    /// builds the scene from the source's resolved paths and drops the
    /// source. Replacing an in-flight source clears the current scene.
    /// Drag-and-drop and the file picker route through whichever source is
    /// currently set, via AssetSource::ingestLocalFile.
    void setSource(std::unique_ptr<AssetSource> source);

    /// Native: blocks until the window closes; returns the exit code.
    /// Emscripten: registers the main loop with the runtime and returns 0
    /// while the loop continues to fire from the event queue.
    int run();

    /// The live App, or nullptr if no `App::Handle` is currently
    /// constructed. Safe to call from any thread that is also live during
    /// the App's lifetime.
    [[nodiscard]] static App *instance();

    /// The currently-installed AssetSource, or nullptr if none. Same
    /// reach-the-live-instance escape hatch as `instance()` — used by the
    /// web upload C export to push browser-fed bytes straight at the
    /// source without an App-level forwarder.
    [[nodiscard]] AssetSource *source() const;

    // Public so the file-scope upload helpers in app.cpp (sokol's drop
    // fetch callback) can access App::Impl. Definition lives entirely in
    // the .cpp; nothing else can manipulate it from this header.
    struct Impl;

    App(PrivateTag, Config cfg);

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

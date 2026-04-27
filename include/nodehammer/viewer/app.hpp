#pragma once

#include <nodehammer/viewer/config.hpp>

#include <memory>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

class AssetSource;

/// Top-level viewer lifecycle. Owns the sokol_app window/event loop, the
/// sokol_gfx render context, and the ImGui state. Native and emscripten
/// builds share the same App; the only divergence is in run().
class App {
  public:
    explicit App(Config cfg);
    ~App();

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    /// Hand the viewer a tessellated scene to render. The App takes a shared
    /// reference; safe to drop the local copy afterwards. Pass nullptr to
    /// clear (revert to demo geometry). May be called before or after run();
    /// scene_renderer uploads lazily on the next frame.
    void set_scene(std::shared_ptr<const RenderScene> scene);

    /// Hand the viewer an asset source. Each frame the App polls it; while
    /// Fetching it draws a progress / placeholder panel, and on Ready it
    /// builds the scene from the source's resolved paths and drops the
    /// source. Replacing an in-flight source clears the current scene.
    /// Drag-and-drop and the file picker route through whichever source is
    /// currently set, via AssetSource::ingest_local_file.
    void set_source(std::unique_ptr<AssetSource> source);

    /// Native: blocks until the window closes; returns the exit code.
    /// Emscripten: registers the main loop with the runtime and returns 0
    /// while the loop continues to fire from the event queue.
    int run();

    // Public so the file-scope upload helpers in app.cpp (the EM_JS shim
    // callback and sokol's drop fetch callback) can access App::Impl.
    // Definition lives entirely in the .cpp; nothing else can manipulate
    // it from this header.
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

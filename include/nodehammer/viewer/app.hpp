#pragma once

#include <nodehammer/viewer/config.hpp>

#include <memory>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

class AssetLoader;

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

    /// Hand the viewer an in-flight asset loader. Each frame the App polls
    /// it; while Fetching it draws a progress panel, and on Ready it builds
    /// the scene from the materialised paths and drops the loader. Used by
    /// the web entry path; native passes a fully-built scene via set_scene.
    void set_loader(std::unique_ptr<AssetLoader> loader);

    /// Native: blocks until the window closes; returns the exit code.
    /// Emscripten: registers the main loop with the runtime and returns 0
    /// while the loop continues to fire from the event queue.
    int run();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

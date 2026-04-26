#pragma once

#include <nodehammer/viewer/config.hpp>

#include <memory>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

/// Top-level viewer lifecycle. Owns the SDL window, bgfx context, and ImGui
/// state. Native and emscripten builds share the same App; the only divergence
/// is in run().
class App {
  public:
    explicit App(Config cfg);
    ~App();

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    /// Hand the viewer a tessellated scene to render. The App takes a shared
    /// reference; safe to drop the local copy afterwards. Pass nullptr to
    /// clear (revert to demo geometry).
    void set_scene(std::shared_ptr<const RenderScene> scene);

    /// Native: blocks until the window closes; returns the exit code.
    /// Emscripten: registers the main loop with the runtime and returns 0
    /// while the loop continues to fire from the event queue.
    int run();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

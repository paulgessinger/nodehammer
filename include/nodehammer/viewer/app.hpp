#pragma once

#include <nodehammer/viewer/config.hpp>

#include <memory>

namespace nodehammer::viewer {

/// Top-level viewer lifecycle. Owns the SDL window, bgfx context, and ImGui
/// state. Stage 1: draws a solid triangle plus an ImGui demo window. Native
/// and emscripten builds share the same App; the only divergence is in run().
class App {
  public:
    explicit App(Config cfg);
    ~App();

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    /// Native: blocks until the window closes; returns the exit code.
    /// Emscripten: registers the main loop with the runtime and returns 0
    /// while the loop continues to fire from the event queue.
    int run();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

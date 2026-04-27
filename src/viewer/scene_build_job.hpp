#pragma once

#include <nodehammer/scene_build.hpp>

#include <atomic>
#include <cstdint>
#include <string>

#ifndef __EMSCRIPTEN__
#include <thread>
#endif

namespace nodehammer::viewer {

/// Runs `build_scene_from_paths` off the main loop. On native the build
/// executes on a worker `std::thread`, so the UI keeps rendering at full
/// frame rate while tessellation churns. On web (no pthreads) the job
/// defers the synchronous build by one `poll()` so the previous frame can
/// paint a "Tessellating…" placeholder before the page freezes for the
/// blocking call.
class SceneBuildJob {
  public:
    SceneBuildJob() = default;
    ~SceneBuildJob();
    SceneBuildJob(const SceneBuildJob &) = delete;
    SceneBuildJob &operator=(const SceneBuildJob &) = delete;

    /// Begin a build. Idempotent — calling `start` a second time before
    /// `take` is invalid; the App is expected to drive a single in-flight
    /// build at a time.
    void start(std::string config_path, std::string input_path);

    /// Drive progress. Returns true once the build has finished (either
    /// with a scene or with diagnostics describing a failure). Until then
    /// callers should keep showing the placeholder UI.
    bool poll();

    /// Move out the build result. Valid only after `poll()` has returned
    /// true. Resets the job to its idle state — a fresh `start` may follow.
    ::nodehammer::SceneBuildResult take();

  private:
    enum class State : uint8_t { Idle, Queued, Running, Done };
    State state_{State::Idle};
    std::string config_path_;
    std::string input_path_;
    ::nodehammer::SceneBuildResult result_;

#ifndef __EMSCRIPTEN__
    std::thread worker_;
    std::atomic<bool> done_{false};
#endif
};

} // namespace nodehammer::viewer

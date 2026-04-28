#pragma once

#include <nodehammer/scene_build.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace nodehammer::viewer {

/// Runs `buildSceneFromPaths` off the main loop, with the tessellation
/// stage broken into cooperative chunks so a long pass doesn't freeze the
/// frame. On native the whole build executes on a worker `std::thread` and
/// `poll` is just a flag check. On web (no pthreads) `poll` runs the
/// upstream stages (config + import + select + dedup) synchronously on
/// its first call — that's the unavoidable stall — and then drives the
/// `TessellationJob` iterator across subsequent polls so the UI keeps
/// rendering during tessellation.
class SceneBuildJob {
  public:
    SceneBuildJob();
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
    /// `budget_ns` caps the per-call wall-clock cost on the web path
    /// (ignored on native, where the worker thread runs unbounded).
    bool poll(uint64_t budget_ns = 8'000'000);

    /// Move out the build result. Valid only after `poll()` has returned
    /// true. Resets the job to its idle state — a fresh `start` may follow.
    ::nodehammer::SceneBuildResult take();

    /// Tessellation progress for UI feedback. Both return 0 until the
    /// tessellation phase is reached; on completion, `processed == total`.
    [[nodiscard]] size_t tessellationTotal() const;
    [[nodiscard]] size_t tessellationProcessed() const;

    /// High-level phase the job is in, so UI can describe what's going on
    /// instead of falling back to an indeterminate animated bar.
    /// `Preparing` covers the (synchronous) config-load / import / select /
    /// dedup stages; `Tessellating` covers the cooperative iterator;
    /// `Finalizing` is the brief packaging step.
    enum class Phase : uint8_t { Idle, Preparing, Tessellating, Finalizing, Done };
    [[nodiscard]] Phase phase() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

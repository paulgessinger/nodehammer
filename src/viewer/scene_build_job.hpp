#pragma once

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/tessellation_job.hpp>

#include <atomic>
#include <cstdint>
#include <string>

#ifndef __EMSCRIPTEN__
#include <thread>
#endif

namespace nodehammer::viewer {

/// Runs `build_scene_from_paths` off the main loop, with the tessellation
/// stage broken into cooperative chunks so a long pass doesn't freeze the
/// frame. On native the whole build executes on a worker `std::thread` and
/// `poll` is just a flag check. On web (no pthreads) `poll` runs the
/// upstream stages (config + import + select + dedup) synchronously on
/// its first call — that's the unavoidable stall — and then drives the
/// `TessellationJob` iterator across subsequent polls so the UI keeps
/// rendering during tessellation.
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
    /// `budget_ns` caps the per-call wall-clock cost on the web path
    /// (ignored on native, where the worker thread runs unbounded).
    bool poll(uint64_t budget_ns = 8'000'000);

    /// Move out the build result. Valid only after `poll()` has returned
    /// true. Resets the job to its idle state — a fresh `start` may follow.
    ::nodehammer::SceneBuildResult take();

    /// Tessellation progress for UI feedback. Both return 0 until the
    /// tessellation phase is reached; on completion, `processed == total`.
    [[nodiscard]] size_t tessellation_total() const;
    [[nodiscard]] size_t tessellation_processed() const;

    /// High-level phase the job is in, so UI can describe what's going on
    /// instead of falling back to an indeterminate animated bar.
    /// `Preparing` covers the (synchronous) config-load / import / select /
    /// dedup stages; `Tessellating` covers the cooperative iterator;
    /// `Finalizing` is the brief packaging step.
    enum class Phase : uint8_t { Idle, Preparing, Tessellating, Finalizing, Done };
    [[nodiscard]] Phase phase() const;

  private:
#ifdef __EMSCRIPTEN__
    enum class State : uint8_t {
        Idle,
        Queued,       // start() called, first poll will paint a "Tessellating…" frame
        PrepPending,  // run upstream stages (config, import, select, dedup) on next poll
        Tessellating, // drive TessellationJob iterator
        Finalizing,   // package result on next poll
        Done,
    };
    State state_{State::Idle};
#else
    enum class State : uint8_t { Idle, Running, Done };
    State state_{State::Idle};
    std::thread worker_;
    std::atomic<bool> done_{false};
#endif

    // The scene/prep state and tessellation job exist on both platforms so
    // the UI can read `tess_job_.{total,processed}_nodes` for a progress
    // bar regardless of build flavor. On native the worker thread owns
    // them while running; the main thread reads only the atomics.
    ::nodehammer::ScenePrepResult prep_;
    ::nodehammer::TessellationJob tess_job_;

    std::string config_path_;
    std::string input_path_;
    ::nodehammer::SceneBuildResult result_;
};

} // namespace nodehammer::viewer

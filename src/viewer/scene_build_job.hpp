#pragma once

#include <nodehammer/scene_build.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace nodehammer::viewer {

/// Drives the validate + select + dedup + tessellate stages off the main
/// loop, with tessellation broken into cooperative chunks so a long pass
/// doesn't freeze the frame. On native the whole build executes on a
/// worker `std::thread` and `poll` is just a flag check. On web (no
/// pthreads) `poll` runs the upstream prep synchronously on its first
/// call and then drives the `TessellationJob` iterator across subsequent
/// polls so the UI keeps rendering during tessellation.
///
/// The viewer always feeds this through bytes-resolved-through-ProjectFs
/// — the BuildSession does the resolve + parse + import dance and then
/// hands the parsed config + imported scene to `start()`.
class SceneBuildJob {
  public:
    SceneBuildJob();
    ~SceneBuildJob();
    SceneBuildJob(const SceneBuildJob &) = delete;
    SceneBuildJob &operator=(const SceneBuildJob &) = delete;

    /// Begin a build. Idempotent — calling `start` a second time before
    /// `take` is invalid; the App is expected to drive a single in-flight
    /// build at a time. The two `_label` strings are diagnostic-only —
    /// they show up in the pre-build log so failures stay attributable.
    ///
    /// When `wedge_cut` is set, the prep stage applies an azimuthal Boolean
    /// wedge cut before tessellation; the caller passes a *pristine* scene so
    /// re-cutting at a new angle re-derives from uncut geometry each build.
    void start(::nodehammer::NHConfig config, ::nodehammer::SemanticScene scene,
               std::string config_label, std::string geometry_label,
               std::optional<::nodehammer::WedgeCutParams> wedge_cut = std::nullopt);

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

    /// Wedge-cut progress for UI feedback. Both return 0 unless a wedge cut
    /// was requested and the `Cutting` phase has been reached; `total` is the
    /// placement count and `processed` grows as the cut is applied.
    [[nodiscard]] size_t wedgeCutTotal() const;
    [[nodiscard]] size_t wedgeCutProcessed() const;

    /// High-level phase the job is in, so UI can describe what's going on
    /// instead of falling back to an indeterminate animated bar.
    /// `Preparing` covers the (synchronous) config-load / import / select /
    /// dedup stages; `Cutting` covers the cooperative azimuthal wedge cut
    /// (only when one was requested); `Tessellating` covers the cooperative
    /// iterator; `Finalizing` is the brief packaging step.
    enum class Phase : uint8_t { Idle, Preparing, Cutting, Tessellating, Finalizing, Done };
    [[nodiscard]] Phase phase() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

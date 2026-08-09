#pragma once

#include <scene_build.hpp>
#include <tessellation/wedge_cut.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace nodehammer::tessellation {

/// Core, GPU-free driver for the `prep → wedge → tessellate` build sequence.
///
/// This is the single reusable primitive behind every viewer build backend
/// (native worker thread, web-cooperative main thread, web-worker second heap)
/// and the synchronous `buildSceneFromPaths` shim. It lifts the state machine
/// that used to be hand-copied at four sites into `nodehammer_lib`, where it
/// depends only on `prepareSceneForTessellationFromInputs`, `WedgeCutJob`,
/// `TessellationJob`, `TessellationPass`, and `SceneBuildResult` — no viewer, no
/// GPU — so it is fully unit-testable in the standard build.
///
/// The five lockstep invariants that used to be enforced only by discipline now
/// live here once: the wedge is always deferred to a `WedgeCutJob` (never run
/// inline in prep); phases advance `Preparing → Cutting → Tessellating →
/// Finalizing`; the wedge counters count placements and the tessellation
/// counters count nodes; the result-packaging tail appends the tessellation
/// diagnostics to prep's and hands the scene back whatever they say, since a
/// node the pass could not mesh is a partial result rather than a failed build
/// (docs/error-model.md); a prep that *cannot* deliver throws, and the throw
/// becomes `SceneBuildResult::failure` because a frame-driven state machine has
/// no caller to unwind to; and prep copies its inputs so the caller's pristine
/// scene is never mutated.
class BuildPipeline {
  public:
    /// Canonical phase enum — the single source of truth shared with
    /// `SceneBuildJob::Phase` (aliased) and asserted against the web-worker's
    /// numeric protocol.
    ///
    /// The numeric values 0..5 are pinned: they match the pre-refactor
    /// `SceneBuildJob::Phase` order and the worker's `kPreparing=1 …
    /// kFinalizing=4` wire codes, so the phase-enum alias and the JS boundary
    /// keep their existing values. `Queued` is an internal pre-prep state (the
    /// first `advance()` burns one frame before running prep) and sits at the
    /// end so it doesn't disturb those pinned values; UI treats it as
    /// `Preparing`.
    enum class Phase : std::uint8_t {
        Idle = 0,
        Preparing = 1,
        Cutting = 2,
        Tessellating = 3,
        Finalizing = 4,
        Done = 5,
        Queued = 6,
    };

    BuildPipeline();
    ~BuildPipeline();
    BuildPipeline(BuildPipeline &&) noexcept;
    BuildPipeline &operator=(BuildPipeline &&) noexcept;
    BuildPipeline(const BuildPipeline &) = delete;
    BuildPipeline &operator=(const BuildPipeline &) = delete;

    /// Begin a build. Inputs are held as `shared_ptr<const>` so the caller only
    /// bumps a refcount here; the deep copy prep consumes is taken lazily inside
    /// `advance()` (this preserves the native "copy off the main thread" and the
    /// cooperative "burn one paint frame before working" behaviours). The wedge
    /// is always deferred to a `WedgeCutJob` (invariant #1) — pass it here and
    /// the pipeline runs it as a separate, progress-reportable phase.
    void start(std::shared_ptr<const config::NHConfig> config,
               std::shared_ptr<const ir::semantic::Scene> scene,
               std::optional<WedgeCutParams> wedgeCut = std::nullopt);

    /// Advance one slice, spending up to `budget_ns` of wall-clock time driving
    /// the wedge/tessellation iterators. Returns true once the build is
    /// complete. The first call after `start()` only transitions
    /// `Queued → Preparing` (returns false) so a frame-driven caller's last
    /// frame paints before the synchronous prep runs; drive-to-completion
    /// callers just spin one extra no-op iteration.
    bool advance(std::uint64_t budget_ns = 8'000'000);

    /// Move out the result. Valid only after `advance()` has returned true.
    /// Resets the pipeline to `Idle`.
    [[nodiscard]] pipeline::SceneBuildResult take();

    [[nodiscard]] Phase phase() const;
    [[nodiscard]] std::size_t tessellationTotal() const;
    [[nodiscard]] std::size_t tessellationProcessed() const;
    [[nodiscard]] std::size_t wedgeCutTotal() const;
    [[nodiscard]] std::size_t wedgeCutProcessed() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::tessellation

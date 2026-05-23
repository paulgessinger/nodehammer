#pragma once

#include <nodehammer/ir/semantic.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace nodehammer {

/// Name of the shared logical volume `applyWedgeCut` binds placements to when
/// they fall entirely inside the removed sector (an empty shape that renders
/// nothing). Its presence in a scene's logVols also serves as a marker that a
/// (non-degenerate) wedge cut was applied — used downstream to recognise that an
/// empty `merge_descendants` result is an expected consequence of the cut rather
/// than a selection/config error.
inline constexpr std::string_view kWedgeEmptyLogVolName = "__wedge_empty";

/// Parameters for an azimuthal "wedge" cut about the global z-axis.
///
/// The *removed* sector runs counter-clockwise from `startDeg` to `endDeg`,
/// with phi measured from the +x axis (the TGeo/Geant4 tube convention). For
/// example `startDeg = 0, endDeg = 90` removes the first quadrant.
struct WedgeCutParams {
    double startDeg{0.0};
    double endDeg{0.0};
    /// Oversize factor for the cutting solid relative to the scene bounds.
    /// The cutter must comfortably enclose every placement it slices; the
    /// default leaves generous headroom.
    double margin{2.0};
};

/// Outcome counts for a wedge-cut pass.
struct WedgeCutStats {
    std::size_t cut{0};       ///< placements straddling the boundary → boolean-cut
    std::size_t cutUnique{0}; ///< distinct cut shapes created (cut placements that share an
                              ///< identical local-frame cut reuse one shape → stay instanced)
    std::size_t emptied{0};   ///< placements fully inside the removed sector → no mesh
    std::size_t kept{0};      ///< placements fully outside → untouched (instancing preserved)
    std::size_t skipped{0};   ///< placements whose shape cannot be bounded → left as-is
    std::size_t pruned{0};    ///< nodes removed as part of fully-cut-away subtrees
};

/// Rewrite `scene` in place to apply an azimuthal wedge cut.
///
/// The transform stays entirely at the semantic level so that the existing
/// `TessellationPass` (and its watertight Manifold boolean path) does the actual
/// geometry work when the scene is lowered. For each placement, a conservative
/// world-space AABB is classified against the removed sector:
///   * **straddling** placements receive a `BooleanSubtraction(originalShape,
///     wedge)` shape expressed in the placement's local frame. Placements whose
///     shape *and* local-frame wedge are identical (e.g. the z-repeated modules
///     of a barrel ladder lying along the cut plane) share one cut shape and
///     therefore stay instanced; only genuinely distinct cuts become new meshes;
///   * placements **fully inside** the removed sector are rebound to one shared
///     empty shape, so they render nothing while remaining instanced;
///   * placements **fully outside** keep their shared shape untouched, so the
///     bulk of the scene stays instanced.
///
/// World transforms are recomputed internally. If the removed sector is
/// degenerate (≈0° or ≈360°) the scene is left unchanged and an all-zero stats
/// value is returned. Outcome counts are reported via the returned stats; the
/// CLI/viewer summarise them — the pass itself emits no diagnostics.
[[nodiscard]] WedgeCutStats applyWedgeCut(SemanticScene &scene, const WedgeCutParams &params);

/// Cooperative, iterator-driven version of `applyWedgeCut`. The cut walks every
/// placement twice — once to size the cutting solid, once to classify and
/// rewrite it — plus a final prune sweep; this class lets the caller spread that
/// work across multiple `advance()` calls so the main thread (web build) stays
/// responsive, and exposes progress counters for UI feedback.
///
/// Usage: `start(scene, params)`, then call `advance(budget_ns)` until it
/// returns true, then `take()` the stats. The cut is applied to `scene`
/// *in place*, so `scene` must outlive the job until `advance` returns true.
/// A degenerate sector (≈0°/≈360°) leaves the scene untouched and makes the
/// first `advance` return true immediately. `applyWedgeCut` is now a thin
/// drive-to-completion shim over this class.
class WedgeCutJob {
  public:
    WedgeCutJob();
    ~WedgeCutJob();
    WedgeCutJob(const WedgeCutJob &) = delete;
    WedgeCutJob &operator=(const WedgeCutJob &) = delete;
    WedgeCutJob(WedgeCutJob &&) noexcept;
    WedgeCutJob &operator=(WedgeCutJob &&) noexcept;

    /// Initialise the job: validate the sector, recompute world transforms and
    /// snapshot the placement list. `scene` is mutated as the job advances.
    void start(SemanticScene &scene, const WedgeCutParams &params);

    /// Apply the cut to pending placements for up to `budget_ns` of wall-clock
    /// time. The atomic unit is one placement (classification plus any cut-shape
    /// build it triggers); the final subtree prune runs as a single slice.
    /// Returns true once the cut is complete and the stats are ready.
    bool advance(std::uint64_t budget_ns = 8'000'000);

    /// Move out the outcome counts. Valid only after `advance` returns true.
    [[nodiscard]] WedgeCutStats take();

    /// Progress for UI feedback. `total` is the placement count (set after
    /// `start`); `processed` grows as the classification sweep runs. Both are
    /// 0 for a degenerate (no-op) cut.
    [[nodiscard]] std::size_t totalPlacements() const;
    [[nodiscard]] std::size_t processedPlacements() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer

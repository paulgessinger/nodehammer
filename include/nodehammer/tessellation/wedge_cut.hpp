#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <cstddef>

namespace nodehammer {

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
/// value is returned.
[[nodiscard]] WedgeCutStats applyWedgeCut(SemanticScene &scene, const WedgeCutParams &params,
                                          DiagnosticList &diags);

} // namespace nodehammer

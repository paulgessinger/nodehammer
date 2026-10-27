#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/semantic.hpp>

namespace nodehammer::geometry {

// Persistent marker: the original structural-node skip_geometry rule must not
// suppress the replacement solid on subsequent builds (including NHB reloads).
inline constexpr const char *kApproximationTag = "nodehammer.geometry";

/// Replace matched semantic subtrees by local-frame convex hulls. Selection must
/// already have run. Outermost matches win; skip_geometry excludes only that
/// node's source solid, never its children. Errors leave the input unchanged.
/// Changes are occurrence-local, with equivalent output definitions deduplicated.
DiagnosticList approximate(ir::semantic::Scene &scene, const config::NHConfig &config);

} // namespace nodehammer::geometry

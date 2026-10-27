#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/semantic.hpp>

namespace nodehammer::geometry {
inline constexpr const char *kDriftChamberTag = "nodehammer_drift_chamber";
inline constexpr const char *kDriftRoleTag = "nodehammer_drift_role";

// Opt-in semantic preparation, before hulls and cutting. Nested gas surfaces
// become empty solids; wire cylinders retain analytic shared geometry. Roles
// persist as metadata, not a specialized renderer or screen-space LOD.
DiagnosticList optimizeDriftChambers(ir::semantic::Scene &scene, const config::NHConfig &config);
} // namespace nodehammer::geometry

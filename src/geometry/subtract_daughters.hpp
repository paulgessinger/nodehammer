#pragma once
#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/semantic.hpp>

namespace nodehammer::geometry {
// Called before selection and other geometry replacements. Builds shared analytic
// Boolean graphs; Manifold evaluates them only when tessellation is requested.
bool hasDaughterSubtraction(const config::NHConfig &config);
DiagnosticList subtractDaughters(ir::semantic::Scene &scene, const config::NHConfig &config);
} // namespace nodehammer::geometry

#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/semantic.hpp>

#include <vector>

namespace nodehammer::ir::semantic {

/// Select physical occurrences directly from shared geometry, without expanding
/// the scene. Replace the scene with the selected shared result in-place.
DiagnosticList select(Scene &scene, const std::vector<config::SelectionRule> &rules, bool hoist);

/// Project a selection into an owned shared scene without expanding or first
/// copying the entire source. The source remains unchanged. Preserves the
/// diagnostics and exception behavior of the in-place selector.
Scene selectCopy(const Scene &scene, const std::vector<config::SelectionRule> &rules, bool hoist,
                 DiagnosticList &diags);

} // namespace nodehammer::ir::semantic

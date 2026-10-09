#pragma once
#include <ir/expanded/scene.hpp>
#include <ir/semantic.hpp>

namespace nodehammer::ir::semantic {
// Preserve the actual stored node tree, not the potentially stale prototype
// daughter lists. One definition per node initially; no equivalence guessing.
// Reject malformed/disconnected trees instead of silently omitting nodes.
[[nodiscard]] Scene fromExpanded(const expanded::Scene &scene);
} // namespace nodehammer::ir::semantic

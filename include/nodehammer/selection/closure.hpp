#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <unordered_set>

namespace nodehammer {

/// Expands a seed set of node IDs according to a ClosurePolicy.
///
///   ClosurePolicy::None        — returns the seed unchanged.
///   ClosurePolicy::Ancestors   — seed + all ancestors up to root.
///   ClosurePolicy::Descendants — seed + all descendants (full subtree, BFS).
///   ClosurePolicy::Full        — seed + ancestors + descendants.
///
/// All IDs in the seed must be present in the scene; expand() throws
/// std::invalid_argument if they are not.  Broken parentId/children
/// references inside the scene (dangling internal pointers) are skipped
/// defensively since those indicate a malformed scene graph rather than a
/// caller error.
///
/// Extension point: if depth-limited descent is needed (e.g. "descendants up
/// to depth 2"), ClosurePolicy should be changed from a plain enum to a
/// variant — e.g. std::variant<ClosureNone, ClosureAncestors,
/// ClosureDescendants{int maxDepth}, ClosureFull> — so the policy can carry
/// parameters.  The TOML representation would be closure = { descendants = 2 }.
/// That refactor touches config_ast.hpp, the config loader/validator, and call
/// sites, but is otherwise mechanical.
namespace ClosureExpander {

[[nodiscard]] std::unordered_set<SemanticNodeId>
expand(const SemanticScene &scene, const std::unordered_set<SemanticNodeId> &seed,
       ClosurePolicy policy);

} // namespace ClosureExpander

} // namespace nodehammer

#pragma once

#include <nodehammer/config/config_ast.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

// ── NodeView ──────────────────────────────────────────────────────────────────
// Lightweight projection of a SemanticNode used by all predicates.
// All string_view members point into the scene or a stable paths map — do not
// store a NodeView beyond the scope where those allocations are live.

struct NodeView {
    std::string_view name;
    std::string_view path; ///< Absolute path from root, e.g. "/world/tracker/module0"
    bool isLeaf{false};    ///< true iff the node has no children
    const std::map<std::string, std::string> *tags{nullptr};
};

// ── Predicate ─────────────────────────────────────────────────────────────────

using Predicate = std::function<bool(const NodeView &)>;

// ── Factory functions ─────────────────────────────────────────────────────────

/// Matches node name against a glob pattern.
/// '*' matches any sequence of characters (name has no '/', so '**' behaves identically).
/// @TODO: add `[abc]` syntax for character class matching.
[[nodiscard]] Predicate makeNameGlobPredicate(std::string pattern);

/// Matches node path against a glob pattern.
/// '*' matches any sequence of non-'/' characters; '**' matches any sequence including '/'.
/// @TODO: add `[abc]` syntax for character class matching.
[[nodiscard]] Predicate makePathGlobPredicate(std::string pattern);

/// True when the node has the given tag key; if value is set, also matches the value.
[[nodiscard]] Predicate makeTagPredicate(std::string key, std::optional<std::string> value);

/// True iff the node is a leaf (no children).
[[nodiscard]] Predicate makeIsLeafPredicate();

/// True iff ALL operand predicates return true (short-circuits on first false).
[[nodiscard]] Predicate makeAndPredicate(std::vector<Predicate> operands);

/// True iff ANY operand predicate returns true (short-circuits on first true).
[[nodiscard]] Predicate makeOrPredicate(std::vector<Predicate> operands);

/// True iff the wrapped predicate returns false.
[[nodiscard]] Predicate makeNotPredicate(Predicate operand);

// ── Compiler ──────────────────────────────────────────────────────────────────

/// Recursively compiles a PredicateExpr AST node into a callable Predicate.
[[nodiscard]] Predicate compilePredicate(const PredicateExpr &expr);

// ── Internal glob utility (exposed for testing) ───────────────────────────────

/// Glob matcher:
///   '*'  matches any sequence of characters that does NOT contain '/'.
///   '**' matches any sequence of characters INCLUDING '/'.
/// Matching is case-sensitive. '?' is treated as a literal character.
[[nodiscard]] bool matchGlob(std::string_view pattern, std::string_view text);

} // namespace nodehammer

#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <unordered_set>
#include <vector>

namespace nodehammer {

// ── SelectionResult ───────────────────────────────────────────────────────────

struct SelectionResult {
    std::unordered_set<SemanticNodeId> kept;
    std::unordered_set<SemanticNodeId> dropped;
    DiagnosticList diags;
};

// ── SelectionEngine ───────────────────────────────────────────────────────────

/// Evaluates [[selection_rules]] against a SemanticScene.
///
/// Rule semantics:
///   - Default disposition: every node is KEPT (no rules = no filtering).
///   - Rules are evaluated in declaration order; LAST matching rule wins.
///   - KeepIf: predicate true → node is kept.
///   - DropIf: predicate true → node is dropped.
///   - scope (optional path glob): if set, the rule is only evaluated for nodes
///     whose path matches the scope. Nodes outside the scope are unaffected.
///   - closure: after per-node evaluation, expands each rule's matched seed by
///     the given policy (None / Ancestors / Descendants / Full).
///
/// Invariants enforced by dryRun:
///   - Descendant invariant: if a node is dropped, all descendants are dropped.
///     When a descendant was explicitly kept by a later rule, NH0400 is emitted.
///   - Root-dropped guard: if the root node ends up in the dropped set, prune()
///     is a no-op and emits NH0401.
class SelectionEngine {
  public:
    explicit SelectionEngine(std::vector<SelectionRule> rules);

    /// Compute which nodes would be kept or dropped without modifying the scene.
    [[nodiscard]] SelectionResult dryRun(const SemanticScene &scene) const;

    /// Remove dropped nodes from the scene in-place and garbage-collect
    /// unreferenced logVols, shapes, and materials.
    ///
    /// If the root node is in the dropped set the scene is left unchanged and
    /// an NH0401 error is emitted.  Returns accumulated diagnostics.
    DiagnosticList prune(SemanticScene &scene) const;

  private:
    std::vector<SelectionRule> rules_;

    /// Core evaluation logic shared by dryRun() and prune().
    [[nodiscard]] SelectionResult evaluate(const SemanticScene &scene) const;
};

} // namespace nodehammer

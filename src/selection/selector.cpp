#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/closure.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/selection/selector.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <format>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer {

namespace {

// Compiled form of one SelectionRule: predicates are already turned into callables.
struct CompiledRule {
    SelectionAction action{};
    std::optional<Predicate> scopePred; // nullopt when rule.scope is absent
    Predicate pred;
    ClosurePolicy closure{};
};

std::vector<CompiledRule> compileRules(const std::vector<SelectionRule> &rules) {
    std::vector<CompiledRule> out;
    out.reserve(rules.size());
    for (const auto &r : rules) {
        CompiledRule cr;
        cr.action = r.action;
        cr.pred = compilePredicate(r.predicate);
        cr.closure = r.closure;
        if (r.scope.has_value()) {
            cr.scopePred = makePathGlobPredicate(*r.scope);
        }
        out.push_back(std::move(cr));
    }
    return out;
}

// BFS reachability: collect all node IDs reachable from root.
std::unordered_set<SemanticNodeId> reachableNodes(const SemanticScene &scene) {
    std::unordered_set<SemanticNodeId> reachable;
    if (scene.nodes.empty() || !scene.nodes.contains(scene.rootId)) {
        return reachable;
    }
    reachable.reserve(scene.nodes.size());
    std::queue<SemanticNodeId> q;
    q.push(scene.rootId);
    while (!q.empty()) {
        const auto id = q.front();
        q.pop();
        if (!scene.nodes.contains(id) || reachable.contains(id)) {
            continue;
        }
        reachable.insert(id);
        for (const auto childId : scene.nodes.at(id).children) {
            q.push(childId);
        }
    }
    return reachable;
}

// Transitively collect all SemanticShapeIds referenced by the given root shapes,
// following left/right operands of boolean shapes.
std::unordered_set<SemanticShapeId>
collectReferencedShapes(const SemanticScene &scene,
                        const std::unordered_set<SemanticShapeId> &roots) {
    std::unordered_set<SemanticShapeId> visited = roots;
    std::queue<SemanticShapeId> q;
    for (const auto &id : roots) {
        q.push(id);
    }
    while (!q.empty()) {
        const auto id = q.front();
        q.pop();
        if (!scene.shapes.contains(id)) {
            continue;
        }
        std::visit(
            [&](const auto &s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, BooleanUnion> ||
                              std::is_same_v<T, BooleanIntersection> ||
                              std::is_same_v<T, BooleanSubtraction>) {
                    if (visited.insert(s.left).second) {
                        q.push(s.left);
                    }
                    if (visited.insert(s.right).second) {
                        q.push(s.right);
                    }
                }
            },
            scene.shapes.at(id).data);
    }
    return visited;
}

} // namespace

// ── SelectionEngine ───────────────────────────────────────────────────────────

SelectionEngine::SelectionEngine(std::vector<SelectionRule> rules, bool hoistOrphans)
    : rules_(std::move(rules)), hoistOrphans_(hoistOrphans) {}

SelectionResult SelectionEngine::evaluate(const SemanticScene &scene) const {
    SelectionResult result;

    if (scene.nodes.empty()) {
        return result;
    }

    const auto compiledRules = compileRules(rules_);
    const auto reachable = reachableNodes(scene);

    // ── Step 1: default disposition = KeepIf for every reachable node. ──────────
    std::unordered_map<SemanticNodeId, SelectionAction> disposition;
    disposition.reserve(reachable.size());
    for (const auto id : reachable) {
        disposition[id] = SelectionAction::KeepIf;
    }

    // ── Step 2: evaluate rules in order (last match wins). ────────────────────
    //
    // For each rule, build a seed of matching node IDs, expand by closure, then
    // write the rule's action to all IDs in the expanded set. Because we process
    // rules in order and write unconditionally, a later rule can overwrite an
    // earlier one — giving last-match-wins semantics.
    for (const auto &cr : compiledRules) {
        std::unordered_set<SemanticNodeId> seed;

        for (const auto id : reachable) {
            const auto &node = scene.nodes.at(id);
            NodeView view;
            view.name = node.name;
            view.path = node.originalPath;
            view.isLeaf = node.children.empty();
            view.tags = &node.tags;

            if (cr.scopePred.has_value() && !(*cr.scopePred)(view)) {
                continue;
            }
            if (!cr.pred(view)) {
                continue;
            }
            seed.insert(id);
        }

        const auto expanded = ClosureExpander::expand(scene, seed, cr.closure);
        for (const auto &id : expanded) {
            disposition[id] = cr.action;
        }
    }

    // ── Step 3: enforce descendant invariant via BFS from root. ───────────────
    //
    // If a parent is dropped and a child is kept, the child has a contradictory
    // disposition: it can't be structurally connected to the scene without its
    // parent. We emit NH0400 and force-drop the child.
    //
    // When hoistOrphans is enabled this invariant is resolved by prune() via
    // re-parenting instead of force-dropping, so we skip the enforcement here.
    if (!hoistOrphans_) {
        std::queue<SemanticNodeId> q;
        if (scene.nodes.contains(scene.rootId)) {
            q.push(scene.rootId);
        }
        while (!q.empty()) {
            const auto id = q.front();
            const auto &node = scene.nodes.at(id);
            q.pop();
            for (const auto childId : node.children) {
                if (!scene.nodes.contains(childId)) {
                    continue;
                }
                if (disposition.at(id) == SelectionAction::DropIf &&
                    disposition.at(childId) == SelectionAction::KeepIf) {
                    result.diags.warn(
                        codes::kWarnSelectionOrphan,
                        std::format("node '{}' kept but parent '{}' dropped — forcing drop",
                                    scene.nodes.at(childId).name, node.name),
                        scene.nodes.at(childId).name);
                    disposition.at(childId) = SelectionAction::DropIf;
                }
                q.push(childId);
            }
        }
    } // end if (!hoistOrphans_)

    // ── Step 4: separate into kept / dropped sets. ────────────────────────────
    for (const auto &[id, action] : disposition) {
        if (action == SelectionAction::DropIf) {
            result.dropped.insert(id);
        } else {
            result.kept.insert(id);
        }
    }

    return result;
}

SelectionResult SelectionEngine::dryRun(const SemanticScene &scene) const {
    return evaluate(scene);
}

DiagnosticList SelectionEngine::prune(SemanticScene &scene) const {
    auto selResult = evaluate(scene);

    // ── Hoist orphans: re-parent KeepIf nodes whose parent is DropIf ─────────────
    // Walk the parentId chain upward to find the nearest KeepIf ancestor and
    // rebase the node's localTransform to that ancestor's world frame.
    // Falls back to root (which is force-kept) when no KeepIf ancestor exists.
    if (hoistOrphans_) {
        // Force-keep root so there is always a valid hoist target.
        selResult.dropped.erase(scene.rootId);
        selResult.kept.insert(scene.rootId);

        for (const auto id : selResult.kept) {
            const SemanticNode &node = scene.nodes.at(id);
            if (!node.parentId.has_value())
                continue; // root itself
            if (selResult.kept.contains(*node.parentId))
                continue; // parent already kept — no hoisting needed

            // Walk up to find nearest kept ancestor.
            SemanticNodeId newParent = scene.rootId;
            auto cur = node.parentId;
            while (cur.has_value()) {
                if (selResult.kept.contains(*cur)) {
                    newParent = *cur;
                    break;
                }
                cur = scene.nodes.at(*cur).parentId;
            }

            // Rebase localTransform to the new parent's world frame.
            const glm::dmat4 &worldNode = scene.nodes.at(id).worldTransform;
            const glm::dmat4 &worldParent = scene.nodes.at(newParent).worldTransform;
            scene.nodes.at(id).localTransform = glm::affineInverse(worldParent) * worldNode;
            scene.nodes.at(id).parentId = newParent;
            // Register with the new parent; the old parent will be pruned away.
            scene.nodes.at(newParent).children.push_back(id);
        }
    }

    // Root-dropped guard: leave scene untouched and emit an error.
    if (selResult.dropped.contains(scene.rootId)) {
        DiagnosticList diags;
        diags.append(selResult.diags);
        diags.error(codes::kErrSelectionRootDropped,
                    "root node is in the dropped set; pruning is a no-op",
                    scene.nodes.at(scene.rootId).name);
        return diags;
    }

    // 1. Remove dropped nodes.
    for (const auto &id : selResult.dropped) {
        scene.nodes.erase(id);
    }

    // 2. Remove dropped IDs from surviving parents' children lists.
    for (auto &[id, node] : scene.nodes) {
        std::erase_if(node.children,
                      [&](SemanticNodeId childId) { return selResult.dropped.contains(childId); });
    }

    // 3. Garbage-collect unreferenced logVols.
    std::unordered_set<SemanticLogVolId> referencedLogVols;
    for (const auto &[id, node] : scene.nodes) {
        referencedLogVols.insert(node.logVolId);
    }
    std::erase_if(scene.logVols,
                  [&](const auto &kv) { return !referencedLogVols.contains(kv.first); });

    // 4. Garbage-collect shapes (transitive: follow boolean operand references).
    std::unordered_set<SemanticShapeId> rootShapes;
    for (const auto &[id, lv] : scene.logVols) {
        rootShapes.insert(lv.shapeId);
    }
    const auto referencedShapes = collectReferencedShapes(scene, rootShapes);
    std::erase_if(scene.shapes,
                  [&](const auto &kv) { return !referencedShapes.contains(kv.first); });

    // 5. Garbage-collect unreferenced materials.
    std::unordered_set<SemanticMaterialId> referencedMaterials;
    for (const auto &[id, lv] : scene.logVols) {
        referencedMaterials.insert(lv.materialId);
    }
    std::erase_if(scene.materials,
                  [&](const auto &kv) { return !referencedMaterials.contains(kv.first); });

    return selResult.diags;
}

} // namespace nodehammer

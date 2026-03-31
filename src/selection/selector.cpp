#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/closure.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/selection/selector.hpp>

#include <format>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer {

namespace {

// Compiled form of one SelectionRule: predicates are already turned into callables.
struct CompiledRule {
    SelectionAction action;
    std::optional<Predicate> scopePred; // nullopt when rule.scope is absent
    Predicate pred;
    ClosurePolicy closure;
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

// BFS path builder: root gets "/<name>", children get parent_path + "/" + name.
std::unordered_map<SemanticNodeId, std::string> buildPaths(const SemanticScene &scene) {
    std::unordered_map<SemanticNodeId, std::string> paths;
    if (scene.nodes.empty() || !scene.nodes.contains(scene.rootId)) {
        return paths;
    }
    paths.reserve(scene.nodes.size());
    paths[scene.rootId] = "/" + scene.nodes.at(scene.rootId).name;

    std::queue<SemanticNodeId> q;
    q.push(scene.rootId);
    while (!q.empty()) {
        const auto &node = scene.nodes.at(q.front());
        q.pop();
        for (const auto childId : node.children) {
            if (!scene.nodes.contains(childId)) {
                continue;
            }
            paths[childId] = paths.at(node.id) + "/" + scene.nodes.at(childId).name;
            q.push(childId);
        }
    }
    return paths;
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

SelectionEngine::SelectionEngine(std::vector<SelectionRule> rules) : rules_(std::move(rules)) {}

SelectionResult SelectionEngine::evaluate(const SemanticScene &scene) const {
    SelectionResult result;

    if (scene.nodes.empty()) {
        return result;
    }

    const auto compiledRules = compileRules(rules_);
    const auto paths = buildPaths(scene);

    // ── Step 1: default disposition = KeepIf for every reachable node. ──────────
    // Initialised from paths (not scene.nodes) so unreachable nodes are never
    // entered — any node in scene.nodes but not in paths is a scene integrity bug.
    std::unordered_map<SemanticNodeId, SelectionAction> disposition;
    disposition.reserve(paths.size());
    for (const auto &[id, path] : paths) {
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

        for (const auto &[id, path] : paths) {
            const auto &node = scene.nodes.at(id);
            NodeView view;
            view.name = node.name;
            view.path = path;
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
    {
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
    }

    // ── Step 4: separate into kept / dropped sets. ────────────────────────────
    // disposition was built from paths, so every ID here is reachable from root.
    // An ID missing from paths at this point would indicate a bug in evaluate().
    for (const auto &[id, action] : disposition) {
        if (!paths.contains(id)) {
            throw std::logic_error("SelectionEngine: disposition contains unreachable node ID — "
                                   "scene integrity violated");
        }
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

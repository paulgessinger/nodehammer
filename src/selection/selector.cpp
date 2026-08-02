#include <diagnostic_codes.hpp>
#include <selection/predicate.hpp>
#include <selection/selector.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <format>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer::selection {

namespace {

// Compiled form of one SelectionRule: predicates are already turned into callables.
struct CompiledRule {
    config::SelectionAction action{};
    std::optional<Predicate> scopePred; // nullopt when rule.scope is absent
    Predicate pred;
};

std::vector<CompiledRule> compileRules(const std::vector<config::SelectionRule> &rules) {
    std::vector<CompiledRule> out;
    out.reserve(rules.size());
    for (const auto &r : rules) {
        CompiledRule cr;
        cr.action = r.action;
        cr.pred = compilePredicate(r.predicate);
        if (r.scope.has_value()) {
            cr.scopePred = makePathGlobPredicate(*r.scope);
        }
        out.push_back(std::move(cr));
    }
    return out;
}

// BFS reachability: collect all node IDs reachable from root, in visitation order.
std::vector<ir::semantic::NodeId> reachableNodes(const ir::semantic::Scene &scene) {
    std::vector<ir::semantic::NodeId> reachable;
    if (scene.nodes.empty() || !scene.nodes.contains(scene.rootId)) {
        return reachable;
    }
    reachable.reserve(scene.nodes.size());
    std::unordered_set<ir::semantic::NodeId> seen;
    seen.reserve(scene.nodes.size());
    std::queue<ir::semantic::NodeId> q;
    q.push(scene.rootId);
    while (!q.empty()) {
        const auto id = q.front();
        q.pop();
        if (!scene.nodes.contains(id) || !seen.insert(id).second) {
            continue;
        }
        reachable.push_back(id);
        for (const auto childId : scene.nodes.at(id).children) {
            q.push(childId);
        }
    }
    return reachable;
}

// Transitively collect all SemanticShapeIds referenced by the given root shapes,
// following left/right operands of boolean shapes.
std::unordered_set<ir::semantic::ShapeId>
collectReferencedShapes(const ir::semantic::Scene &scene,
                        const std::unordered_set<ir::semantic::ShapeId> &roots) {
    std::unordered_set<ir::semantic::ShapeId> visited = roots;
    std::queue<ir::semantic::ShapeId> q;
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
                if constexpr (std::is_same_v<T, ir::semantic::BooleanUnion> ||
                              std::is_same_v<T, ir::semantic::BooleanIntersection> ||
                              std::is_same_v<T, ir::semantic::BooleanSubtraction>) {
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

SelectionEngine::SelectionEngine(std::vector<config::SelectionRule> rules, bool hoistOrphans)
    : rules_(std::move(rules)), hoistOrphans_(hoistOrphans) {}

SelectionResult SelectionEngine::evaluate(const ir::semantic::Scene &scene) const {
    SelectionResult result;

    if (scene.nodes.empty()) {
        return result;
    }

    const auto compiledRules = compileRules(rules_);
    const auto reachable = reachableNodes(scene);

    // ── Step 1: precompute NodeViews + parallel disposition vector. ───────────
    //
    // NodeView construction is rule-independent, so building it once up front
    // avoids O(rules × nodes) rebuilds and collapses the per-iteration
    // scene.nodes / scene.logVols / scene.materials hash-map traffic.
    //
    // `disposition` is indexed parallel to `nodeEntries` — a flat vector beats
    // an unordered_map<semantic::NodeId, …> on every metric (no hashing, no node
    // allocations, contiguous access). Default-initialized SelectionAction is
    // KeepIf (enum value 0), which is exactly the Step-1 default we want.
    struct NodeEntry {
        ir::semantic::NodeId id;
        NodeView view;
    };
    std::vector<NodeEntry> nodeEntries;
    nodeEntries.reserve(reachable.size());
    for (const auto id : reachable) {
        const auto &node = scene.nodes.at(id);
        std::string_view matName;
        if (auto lvIt = scene.logVols.find(node.logVolId); lvIt != scene.logVols.end()) {
            if (auto matIt = scene.materials.find(lvIt->second.materialId);
                matIt != scene.materials.end()) {
                matName = matIt->second.name;
            }
        }
        NodeView view;
        view.name = node.name;
        view.path = node.originalPath;
        view.materialName = matName;
        view.isLeaf = node.children.empty();
        view.tags = &node.tags;
        nodeEntries.push_back({id, view});
    }
    std::vector<config::SelectionAction> disposition(nodeEntries.size(),
                                                     config::SelectionAction::KeepIf);

    // ── Step 2: evaluate rules in order (last match wins). ────────────────────
    //
    // For each rule, find matching node IDs and write the rule's action.
    // Because we process rules in order and write unconditionally, a later
    // rule can overwrite an earlier one — giving last-match-wins semantics.
    for (const auto &cr : compiledRules) {
        for (std::size_t i = 0; i < nodeEntries.size(); ++i) {
            const auto &entry = nodeEntries[i];
            if (cr.scopePred.has_value() && !(*cr.scopePred)(entry.view)) {
                continue;
            }
            if (!cr.pred(entry.view)) {
                continue;
            }
            disposition[i] = cr.action;
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
        // Build an id → index map once so the BFS can look up dispositions by ID.
        std::unordered_map<ir::semantic::NodeId, std::size_t> idToIndex;
        idToIndex.reserve(nodeEntries.size());
        for (std::size_t i = 0; i < nodeEntries.size(); ++i) {
            idToIndex.emplace(nodeEntries[i].id, i);
        }

        std::queue<ir::semantic::NodeId> q;
        if (scene.nodes.contains(scene.rootId)) {
            q.push(scene.rootId);
        }
        while (!q.empty()) {
            const auto id = q.front();
            const auto &node = scene.nodes.at(id);
            q.pop();
            const auto parentIt = idToIndex.find(id);
            if (parentIt == idToIndex.end()) {
                continue;
            }
            for (const auto childId : node.children) {
                if (!scene.nodes.contains(childId)) {
                    continue;
                }
                const auto childIt = idToIndex.find(childId);
                if (childIt != idToIndex.end() &&
                    disposition[parentIt->second] == config::SelectionAction::DropIf &&
                    disposition[childIt->second] == config::SelectionAction::KeepIf) {
                    result.diags.warn(
                        codes::kWarnSelectionOrphan,
                        std::format("node '{}' kept but parent '{}' dropped -- forcing drop",
                                    scene.nodes.at(childId).name, node.name),
                        scene.nodes.at(childId).name);
                    disposition[childIt->second] = config::SelectionAction::DropIf;
                }
                q.push(childId);
            }
        }
    } // end if (!hoistOrphans_)

    // ── Step 4: separate into kept / dropped sets. ────────────────────────────
    for (std::size_t i = 0; i < nodeEntries.size(); ++i) {
        if (disposition[i] == config::SelectionAction::DropIf) {
            result.dropped.insert(nodeEntries[i].id);
        } else {
            result.kept.insert(nodeEntries[i].id);
        }
    }

    return result;
}

SelectionResult SelectionEngine::dryRun(const ir::semantic::Scene &scene) const {
    return evaluate(scene);
}

diagnostics::List SelectionEngine::prune(ir::semantic::Scene &scene) const {
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
            const ir::semantic::Node &node = scene.nodes.at(id);
            if (!node.parentId.has_value())
                continue; // root itself
            if (selResult.kept.contains(*node.parentId))
                continue; // parent already kept — no hoisting needed

            // Walk up to find nearest kept ancestor.
            ir::semantic::NodeId newParent = scene.rootId;
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
        diagnostics::List diags;
        diags.append(selResult.diags);
        diags.error(codes::kFatalSelectionRootDropped,
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
        std::erase_if(node.children, [&](ir::semantic::NodeId childId) {
            return selResult.dropped.contains(childId);
        });
    }

    // 3. Garbage-collect unreferenced logVols. Logical volumes can carry source-level
    // daughter prototypes even when the corresponding placement nodes were pruned/hoisted,
    // so keep the transitive daughter logVol closure for every surviving node logVol.
    std::unordered_set<ir::semantic::LogVolId> referencedLogVols;
    for (const auto &[id, node] : scene.nodes) {
        if (!referencedLogVols.insert(node.logVolId).second) {
            continue;
        }
        std::queue<ir::semantic::LogVolId> q;
        q.push(node.logVolId);
        while (!q.empty()) {
            const auto lvId = q.front();
            q.pop();
            if (!scene.logVols.contains(lvId)) {
                continue;
            }
            for (const auto &daughter : scene.logVols.at(lvId).daughters) {
                if (referencedLogVols.insert(daughter.logVolId).second) {
                    q.push(daughter.logVolId);
                }
            }
        }
    }
    std::erase_if(scene.logVols,
                  [&](const auto &kv) { return !referencedLogVols.contains(kv.first); });

    // 4. Garbage-collect shapes (transitive: follow boolean operand references).
    std::unordered_set<ir::semantic::ShapeId> rootShapes;
    for (const auto &[id, lv] : scene.logVols) {
        rootShapes.insert(lv.shapeId);
    }
    const auto referencedShapes = collectReferencedShapes(scene, rootShapes);
    std::erase_if(scene.shapes,
                  [&](const auto &kv) { return !referencedShapes.contains(kv.first); });

    // 5. Garbage-collect unreferenced materials.
    std::unordered_set<ir::semantic::MaterialId> referencedMaterials;
    for (const auto &[id, lv] : scene.logVols) {
        referencedMaterials.insert(lv.materialId);
    }
    std::erase_if(scene.materials,
                  [&](const auto &kv) { return !referencedMaterials.contains(kv.first); });

    return selResult.diags;
}

} // namespace nodehammer::selection

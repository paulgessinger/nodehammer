#include <catch2/catch_test_macros.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/selector.hpp>

#include <unordered_set>

using namespace nodehammer;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a 3-level scene: root("world") → mid("tracker") → leaf("sensor")
static auto makeThreeLevelScene() {
    SemanticScene scene;

    SemanticShapeId shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, BoxShape{10, 10, 10}};

    SemanticMaterialId matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};

    auto makeLv = [&](const std::string &name) {
        SemanticLogVolId id = scene.nextLogVolId();
        scene.logVols[id] = {id, name, shapeId, matId};
        return id;
    };

    SemanticLogVolId worldLv = makeLv("world_lv");
    SemanticLogVolId trackerLv = makeLv("tracker_lv");
    SemanticLogVolId sensorLv = makeLv("sensor_lv");

    SemanticNodeId rootId = scene.nextNodeId();
    SemanticNodeId trackerId = scene.nextNodeId();
    SemanticNodeId sensorId = scene.nextNodeId();

    SemanticNode root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = worldLv;
    root.children = {trackerId};
    scene.nodes[rootId] = root;

    SemanticNode tracker;
    tracker.id = trackerId;
    tracker.name = "tracker";
    tracker.logVolId = trackerLv;
    tracker.parentId = rootId;
    tracker.children = {sensorId};
    scene.nodes[trackerId] = tracker;

    SemanticNode sensor;
    sensor.id = sensorId;
    sensor.name = "sensor";
    sensor.logVolId = sensorLv;
    sensor.parentId = trackerId;
    scene.nodes[sensorId] = sensor;

    scene.rootId = rootId;
    scene.computeWorldTransforms();

    struct Result {
        SemanticScene scene;
        SemanticNodeId rootId, trackerId, sensorId;
    };
    return Result{std::move(scene), rootId, trackerId, sensorId};
}

// ── dryRun: basic disposition ─────────────────────────────────────────────────

TEST_CASE("SelectionEngine: no rules — all nodes kept", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionEngine eng{{}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.kept.size() == 3);
    REQUIRE(result.dropped.empty());
    REQUIRE(result.diags.empty());
}

TEST_CASE("SelectionEngine: drop_if name match drops node and its children",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule rule;
    rule.action = SelectionAction::DropIf;
    rule.predicate = PredicateExpr{NameGlobPredicate{"tracker"}};
    rule.closure = ClosurePolicy::None;

    SelectionEngine eng{{rule}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.dropped.contains(trackerId));
    // sensor is a descendant of tracker — must also be dropped (descendant invariant)
    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.kept.contains(rootId));
}

TEST_CASE("SelectionEngine: keep_if by path glob keeps only matching subtree",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // First drop everything, then keep root and tracker subtree.
    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};
    dropAll.closure = ClosurePolicy::None;

    SelectionRule keepTracker;
    keepTracker.action = SelectionAction::KeepIf;
    keepTracker.predicate = PredicateExpr{PathGlobPredicate{"/world/tracker"}};
    keepTracker.closure = ClosurePolicy::Ancestors; // also keep root

    SelectionEngine eng{{dropAll, keepTracker}};
    auto result = eng.dryRun(scene);

    // root kept via Ancestors closure, tracker explicitly kept
    REQUIRE(result.kept.contains(rootId));
    REQUIRE(result.kept.contains(trackerId));
    // sensor was dropped by first rule and not re-kept
    REQUIRE(result.dropped.contains(sensorId));
}

TEST_CASE("SelectionEngine: later rule overrides earlier rule", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule keepAll;
    keepAll.action = SelectionAction::KeepIf;
    keepAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionRule dropSensor;
    dropSensor.action = SelectionAction::DropIf;
    dropSensor.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{keepAll, dropSensor}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.kept.contains(trackerId));
    REQUIRE(result.kept.contains(rootId));
}

TEST_CASE("SelectionEngine: orphan warning emitted when kept child has dropped parent",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // Drop tracker, then explicitly keep sensor (contradictory config).
    SelectionRule dropTracker;
    dropTracker.action = SelectionAction::DropIf;
    dropTracker.predicate = PredicateExpr{NameGlobPredicate{"tracker"}};

    SelectionRule keepSensor;
    keepSensor.action = SelectionAction::KeepIf;
    keepSensor.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{dropTracker, keepSensor}};
    auto result = eng.dryRun(scene);

    // Sensor is force-dropped because its parent (tracker) is dropped.
    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.dropped.contains(trackerId));

    // Orphan warning should have been emitted.
    bool hasOrphanWarn = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == codes::kWarnSelectionOrphan) {
            hasOrphanWarn = true;
        }
    }
    REQUIRE(hasOrphanWarn);
}

TEST_CASE("SelectionEngine: dryRun does not modify the scene", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    const std::size_t nodesBefore = scene.nodes.size();
    const std::size_t shapesBefore = scene.shapes.size();

    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionEngine eng{{dropAll}};
    [[maybe_unused]] auto _ = eng.dryRun(scene); // must not modify scene

    REQUIRE(scene.nodes.size() == nodesBefore);
    REQUIRE(scene.shapes.size() == shapesBefore);
}

// ── prune ─────────────────────────────────────────────────────────────────────

TEST_CASE("SelectionEngine: prune removes dropped nodes from scene", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule dropSensor;
    dropSensor.action = SelectionAction::DropIf;
    dropSensor.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{dropSensor}};
    auto diags = eng.prune(scene);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(scene.nodes.size() == 2);
    REQUIRE_FALSE(scene.nodes.contains(sensorId));
    REQUIRE(scene.nodes.contains(trackerId));
    REQUIRE(scene.nodes.contains(rootId));
}

TEST_CASE("SelectionEngine: prune fixes parent children lists", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule dropSensor;
    dropSensor.action = SelectionAction::DropIf;
    dropSensor.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{dropSensor}};
    eng.prune(scene);

    // tracker's children list should no longer contain sensorId
    REQUIRE(scene.nodes.at(trackerId).children.empty());
}

TEST_CASE("SelectionEngine: prune garbage-collects unreferenced logVols and materials",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // All 3 nodes have distinct logVols and share one material.
    const std::size_t logVolsBefore = scene.logVols.size();
    REQUIRE(logVolsBefore == 3);

    SelectionRule dropTracker;
    dropTracker.action = SelectionAction::DropIf;
    dropTracker.predicate = PredicateExpr{NameGlobPredicate{"tracker"}};
    dropTracker.closure = ClosurePolicy::Descendants; // also drops sensor

    SelectionEngine eng{{dropTracker}};
    eng.prune(scene);

    // Only world's logVol should remain.
    REQUIRE(scene.logVols.size() == 1);
    // Material is still referenced by world's logVol.
    REQUIRE(scene.materials.size() == 1);
}

TEST_CASE("SelectionEngine: prune with root dropped is a no-op and emits NH0401",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionEngine eng{{dropAll}};

    const std::size_t nodesBefore = scene.nodes.size();
    auto diags = eng.prune(scene);

    REQUIRE(scene.nodes.size() == nodesBefore); // scene untouched
    REQUIRE(diags.hasErrors());

    bool hasRootErr = false;
    for (const auto &d : diags.items()) {
        if (d.code == codes::kErrSelectionRootDropped) {
            hasRootErr = true;
        }
    }
    REQUIRE(hasRootErr);
}

TEST_CASE("SelectionEngine: prune produces structurally sound scene", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule dropSensor;
    dropSensor.action = SelectionAction::DropIf;
    dropSensor.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{dropSensor}};
    eng.prune(scene);

    // BFS from root should reach all remaining nodes.
    std::unordered_set<SemanticNodeId> visited;
    scene.visitBFS([&](const auto &node) { visited.insert(node.id); });
    REQUIRE(visited.size() == scene.nodes.size());
}

TEST_CASE("SelectionEngine: multi-level descendant cascade forces all descendants dropped",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // Drop only the root — tracker and sensor must both be force-dropped via
    // the descendant invariant, even though only root was explicitly dropped.
    SelectionRule dropRoot;
    dropRoot.action = SelectionAction::DropIf;
    dropRoot.predicate = PredicateExpr{NameGlobPredicate{"world"}};

    SelectionEngine eng{{dropRoot}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.dropped.contains(rootId));
    REQUIRE(result.dropped.contains(trackerId));
    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.kept.empty());
}

TEST_CASE("SelectionEngine: node unreachable from root is excluded from evaluation",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // Insert an orphan node that exists in scene.nodes but is not reachable
    // from root via any children list.
    SemanticNodeId orphanId = scene.nextNodeId();
    SemanticNode orphan;
    orphan.id = orphanId;
    orphan.name = "orphan";
    orphan.logVolId = scene.nodes.at(rootId).logVolId; // reuse existing logVol
    scene.nodes[orphanId] = orphan;

    // A rule that would match "orphan" by name — but since it's unreachable
    // from root it should not appear in either kept or dropped.
    SelectionRule dropOrphan;
    dropOrphan.action = SelectionAction::DropIf;
    dropOrphan.predicate = PredicateExpr{NameGlobPredicate{"orphan"}};

    SelectionEngine eng{{dropOrphan}};
    auto result = eng.dryRun(scene);

    REQUIRE_FALSE(result.kept.contains(orphanId));
    REQUIRE_FALSE(result.dropped.contains(orphanId));
    // The reachable nodes are unaffected.
    REQUIRE(result.kept.contains(rootId));
    REQUIRE(result.kept.contains(trackerId));
    REQUIRE(result.kept.contains(sensorId));
}

// ── NodeView population ───────────────────────────────────────────────────────

TEST_CASE("SelectionEngine: NodeView.isLeaf is true only for nodes with no children",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // Drop all leaves — only sensor has no children, so only it should be dropped.
    SelectionRule dropLeaves;
    dropLeaves.action = SelectionAction::DropIf;
    dropLeaves.predicate = PredicateExpr{IsLeafPredicate{}};

    SelectionEngine eng{{dropLeaves}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.kept.contains(trackerId));
    REQUIRE(result.kept.contains(rootId));
}

TEST_CASE("SelectionEngine: NodeView.path reflects full ancestor chain", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // Match sensor by its exact absolute path.
    SelectionRule dropSensor;
    dropSensor.action = SelectionAction::DropIf;
    dropSensor.predicate = PredicateExpr{PathGlobPredicate{"/world/tracker/sensor"}};

    SelectionEngine eng{{dropSensor}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.kept.contains(trackerId));
    REQUIRE(result.kept.contains(rootId));
}

TEST_CASE("SelectionEngine: scope restricts which nodes a rule evaluates",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // Drop nodes named "sensor" only within the /world/tracker scope.
    SelectionRule rule;
    rule.action = SelectionAction::DropIf;
    rule.scope = "/world/tracker/**";
    rule.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{rule}};
    auto result = eng.dryRun(scene);

    REQUIRE(result.dropped.contains(sensorId));
    REQUIRE(result.kept.contains(trackerId));
    REQUIRE(result.kept.contains(rootId));
}

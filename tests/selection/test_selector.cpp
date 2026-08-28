#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <diagnostic_codes.hpp>
#include <ir/glm_interop.hpp>
#include <ir/synthetic/semantic/importer.hpp>
#include <selection/selector.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <unordered_set>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::config;
using namespace nodehammer::selection;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a 3-level scene: root("world") → mid("tracker") → leaf("sensor")
static auto makeThreeLevelScene() {
    ir::semantic::Scene scene;

    ir::semantic::ShapeId shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, ir::semantic::BoxShape{10, 10, 10}};

    ir::semantic::MaterialId matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};

    auto makeLv = [&](const std::string &name) {
        ir::semantic::LogVolId id = scene.nextLogVolId();
        scene.logVols[id] = {id, name, shapeId, matId};
        return id;
    };

    ir::semantic::LogVolId worldLv = makeLv("world_lv");
    ir::semantic::LogVolId trackerLv = makeLv("tracker_lv");
    ir::semantic::LogVolId sensorLv = makeLv("sensor_lv");

    ir::semantic::NodeId rootId = scene.nextNodeId();
    ir::semantic::NodeId trackerId = scene.nextNodeId();
    ir::semantic::NodeId sensorId = scene.nextNodeId();

    ir::semantic::Node root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = worldLv;
    root.children = {trackerId};
    scene.nodes[rootId] = root;

    ir::semantic::Node tracker;
    tracker.id = trackerId;
    tracker.name = "tracker";
    tracker.logVolId = trackerLv;
    tracker.parentId = rootId;
    tracker.children = {sensorId};
    scene.nodes[trackerId] = tracker;

    ir::semantic::Node sensor;
    sensor.id = sensorId;
    sensor.name = "sensor";
    sensor.logVolId = sensorLv;
    sensor.parentId = trackerId;
    scene.nodes[sensorId] = sensor;

    scene.rootId = rootId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();

    struct Result {
        ir::semantic::Scene scene;
        ir::semantic::NodeId rootId, trackerId, sensorId;
    };
    return Result{std::move(scene), rootId, trackerId, sensorId};
}

// ── dryRun: basic disposition ─────────────────────────────────────────────────

TEST_CASE("SelectionEngine: no rules -- all nodes kept", "[selection][selector]") {
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

    // Keep root and tracker explicitly (no closure expansion).
    SelectionRule keepRoot;
    keepRoot.action = SelectionAction::KeepIf;
    keepRoot.predicate = PredicateExpr{NameGlobPredicate{"world"}};

    SelectionRule keepTracker;
    keepTracker.action = SelectionAction::KeepIf;
    keepTracker.predicate = PredicateExpr{PathGlobPredicate{"/world/tracker"}};

    SelectionEngine eng{{dropAll, keepRoot, keepTracker}};
    auto result = eng.dryRun(scene);

    // root and tracker explicitly kept
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

    // Drop tracker — sensor is force-dropped via descendant invariant.
    SelectionRule dropTracker;
    dropTracker.action = SelectionAction::DropIf;
    dropTracker.predicate = PredicateExpr{NameGlobPredicate{"tracker"}};

    SelectionEngine eng{{dropTracker}};
    eng.prune(scene);

    // Only world's logVol should remain.
    REQUIRE(scene.logVols.size() == 1);
    // Material is still referenced by world's logVol.
    REQUIRE(scene.materials.size() == 1);
}

TEST_CASE("SelectionEngine: prune keeps source daughter logVol closure", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    const auto worldLv = scene.nodes.at(rootId).logVolId;
    const auto trackerLv = scene.nodes.at(trackerId).logVolId;
    const auto sensorLv = scene.nodes.at(sensorId).logVolId;

    scene.logVols.at(worldLv).daughters.push_back({"tracker", trackerLv, nodehammer::ir::Mat4{}});
    scene.logVols.at(trackerLv).daughters.push_back({"sensor", sensorLv, nodehammer::ir::Mat4{}});

    // Drop tracker — sensor is force-dropped via descendant invariant.
    SelectionRule dropTracker;
    dropTracker.action = SelectionAction::DropIf;
    dropTracker.predicate = PredicateExpr{NameGlobPredicate{"tracker"}};

    SelectionEngine eng{{dropTracker}};
    eng.prune(scene);

    REQUIRE(scene.nodes.size() == 1);
    REQUIRE(scene.logVols.contains(worldLv));
    REQUIRE(scene.logVols.contains(trackerLv));
    REQUIRE(scene.logVols.contains(sensorLv));
}

TEST_CASE("SelectionEngine: prune throws NH0401 when the rules drop the root",
          "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionEngine eng{{dropAll}};

    const std::size_t nodesBefore = scene.nodes.size();
    try {
        (void)eng.prune(scene);
        FAIL("expected a throw");
    } catch (const nodehammer::Error &e) {
        // Fatal because the scene `prune` could return is the one with the
        // rules *not* applied — see docs/error-model.md. The caller's scene is
        // left untouched, since nothing was removed before the guard fired.
        REQUIRE(e.code() == codes::kFatalSelectionRootDropped);
        REQUIRE(scene.nodes.size() == nodesBefore);
    }
}

TEST_CASE("SelectionEngine: prune produces structurally sound scene", "[selection][selector]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    SelectionRule dropSensor;
    dropSensor.action = SelectionAction::DropIf;
    dropSensor.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};

    SelectionEngine eng{{dropSensor}};
    eng.prune(scene);

    // BFS from root should reach all remaining nodes.
    std::unordered_set<ir::semantic::NodeId> visited;
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
    ir::semantic::NodeId orphanId = scene.nextNodeId();
    ir::semantic::Node orphan;
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

// ── Hoist helpers ─────────────────────────────────────────────────────────────

// Build a minimal semantic::Scene with given names and double-precision
// translation-only local transforms. Returns node IDs in insertion order.
// Parent chain: nodes[0] is root, nodes[i] is parent of nodes[i+1].
static ir::semantic::Scene
makeLinearScene(const std::vector<std::string> &names,
                const std::vector<glm::dvec3> &translations) // local translation per node
{
    ir::semantic::Scene scene;

    ir::semantic::ShapeId shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, ir::semantic::BoxShape{1, 1, 1}};
    ir::semantic::MaterialId matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};
    ir::semantic::LogVolId lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "lv", shapeId, matId};

    std::vector<ir::semantic::NodeId> ids;
    ids.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        ir::semantic::NodeId id = scene.nextNodeId();
        ids.push_back(id);
        ir::semantic::Node node;
        node.id = id;
        node.name = names[i];
        node.logVolId = lvId;
        node.localTransform =
            nodehammer::ir::fromGlm(glm::translate(glm::dmat4(1.0), translations[i]));
        if (i > 0) {
            node.parentId = ids[i - 1];
            scene.nodes.at(ids[i - 1]).children.push_back(id);
        }
        scene.nodes[id] = node;
    }
    scene.rootId = ids[0];
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

// ── Hoist tests ───────────────────────────────────────────────────────────────

TEST_CASE("SelectionEngine hoist: orphan re-parented to nearest kept ancestor",
          "[selection][selector][hoist]") {
    // root(0,0,0) -> A(10,0,0) -> B(5,0,0) [dropped] -> C(3,0,0)
    // world(A)=10, world(B)=15, world(C)=18
    // After hoist: C.parentId = A, C.localTransform = translate(8,0,0)
    auto scene =
        makeLinearScene({"root", "A", "B", "C"}, {{0, 0, 0}, {10, 0, 0}, {5, 0, 0}, {3, 0, 0}});
    ir::semantic::NodeId aId, bId, cId;
    for (const auto &[id, n] : scene.nodes) {
        if (n.name == "A")
            aId = id;
        if (n.name == "B")
            bId = id;
        if (n.name == "C")
            cId = id;
    }

    // drop_if * , then keep_if A and C explicitly
    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionRule keepA;
    keepA.action = SelectionAction::KeepIf;
    keepA.predicate = PredicateExpr{NameGlobPredicate{"A"}};

    SelectionRule keepC;
    keepC.action = SelectionAction::KeepIf;
    keepC.predicate = PredicateExpr{NameGlobPredicate{"C"}};

    SelectionEngine eng{{dropAll, keepA, keepC}, /*hoistOrphans=*/true};
    auto diags = eng.prune(scene);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(scene.nodes.contains(aId));
    REQUIRE(scene.nodes.contains(cId));
    REQUIRE_FALSE(scene.nodes.contains(bId));

    // C is now a child of A
    REQUIRE(scene.nodes.at(cId).parentId == aId);
    REQUIRE_FALSE(scene.nodes.at(aId).children.empty());

    // C's new localTransform should translate by (8,0,0) relative to A's world
    const glm::dvec3 t = glm::dvec3(nodehammer::ir::toGlm(scene.nodes.at(cId).localTransform)[3]);
    REQUIRE(t.x == Catch::Approx(8.0));
    REQUIRE(t.y == Catch::Approx(0.0));
    REQUIRE(t.z == Catch::Approx(0.0));
}

TEST_CASE("SelectionEngine hoist: no kept ancestor falls back to root",
          "[selection][selector][hoist]") {
    // root(0,0,0) -> A(5,0,0) [dropped] -> B(3,0,0) [kept]
    // world(B) = (8,0,0). After hoist: B.parentId = root, localTransform = translate(8,0,0)
    auto scene = makeLinearScene({"root", "A", "B"}, {{0, 0, 0}, {5, 0, 0}, {3, 0, 0}});
    auto rootId = scene.rootId;
    ir::semantic::NodeId aId, bId;
    for (const auto &[id, n] : scene.nodes) {
        if (n.name == "A")
            aId = id;
        if (n.name == "B")
            bId = id;
    }

    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionRule keepB;
    keepB.action = SelectionAction::KeepIf;
    keepB.predicate = PredicateExpr{NameGlobPredicate{"B"}};

    SelectionEngine eng{{dropAll, keepB}, /*hoistOrphans=*/true};
    auto diags = eng.prune(scene);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(scene.nodes.contains(rootId));
    REQUIRE(scene.nodes.contains(bId));
    REQUIRE_FALSE(scene.nodes.contains(aId));

    REQUIRE(scene.nodes.at(bId).parentId == rootId);

    const glm::dvec3 t = glm::dvec3(nodehammer::ir::toGlm(scene.nodes.at(bId).localTransform)[3]);
    REQUIRE(t.x == Catch::Approx(8.0));
    REQUIRE(t.y == Catch::Approx(0.0));
    REQUIRE(t.z == Catch::Approx(0.0));
}

TEST_CASE("SelectionEngine hoist: subtree of hoisted node is preserved",
          "[selection][selector][hoist]") {
    // root -> A(5,0,0) [dropped] -> B(3,0,0) [kept] -> C(2,0,0) [kept]
    // B hoists to root; C stays child of B with its original localTransform.
    auto scene =
        makeLinearScene({"root", "A", "B", "C"}, {{0, 0, 0}, {5, 0, 0}, {3, 0, 0}, {2, 0, 0}});
    auto rootId = scene.rootId;
    ir::semantic::NodeId aId, bId, cId;
    for (const auto &[id, n] : scene.nodes) {
        if (n.name == "A")
            aId = id;
        if (n.name == "B")
            bId = id;
        if (n.name == "C")
            cId = id;
    }

    SelectionRule dropAll;
    dropAll.action = SelectionAction::DropIf;
    dropAll.predicate = PredicateExpr{NameGlobPredicate{"*"}};

    SelectionRule keepB;
    keepB.action = SelectionAction::KeepIf;
    keepB.predicate = PredicateExpr{NameGlobPredicate{"B"}};

    SelectionRule keepC;
    keepC.action = SelectionAction::KeepIf;
    keepC.predicate = PredicateExpr{NameGlobPredicate{"C"}};

    SelectionEngine eng{{dropAll, keepB, keepC}, /*hoistOrphans=*/true};
    auto diags = eng.prune(scene);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(scene.nodes.contains(aId));
    REQUIRE(scene.nodes.contains(bId));
    REQUIRE(scene.nodes.contains(cId));

    // B is now a child of root
    REQUIRE(scene.nodes.at(bId).parentId == rootId);
    // C is still a child of B with its original local translation of (2,0,0)
    REQUIRE(scene.nodes.at(cId).parentId == bId);
    const glm::dvec3 tc = glm::dvec3(nodehammer::ir::toGlm(scene.nodes.at(cId).localTransform)[3]);
    REQUIRE(tc.x == Catch::Approx(2.0));
}

TEST_CASE("SelectionEngine hoist: no effect when all parents are kept",
          "[selection][selector][hoist]") {
    auto [scene, rootId, trackerId, sensorId] = makeThreeLevelScene();

    // No drop rules — everything kept. Hoist should be a no-op.
    SelectionEngine eng{{}, /*hoistOrphans=*/true};
    auto diags = eng.prune(scene);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(scene.nodes.contains(rootId));
    REQUIRE(scene.nodes.contains(trackerId));
    REQUIRE(scene.nodes.contains(sensorId));
    REQUIRE(scene.nodes.at(sensorId).parentId == trackerId);
}

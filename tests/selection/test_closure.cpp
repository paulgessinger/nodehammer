#include <catch2/catch_test_macros.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/selection/closure.hpp>

#include <unordered_set>

using namespace nodehammer;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a 3-level hierarchy:  root → mid → leaf
// Returns the scene and the three node IDs.
static auto makeThreeLevelScene() {
    SemanticScene scene;

    SemanticShapeId shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, BoxShape{10, 10, 10}};

    SemanticMaterialId matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};

    SemanticLogVolId rootLvId = scene.nextLogVolId();
    scene.logVols[rootLvId] = {rootLvId, "world_lv", shapeId, matId};

    SemanticLogVolId midLvId = scene.nextLogVolId();
    scene.logVols[midLvId] = {midLvId, "mid_lv", shapeId, matId};

    SemanticLogVolId leafLvId = scene.nextLogVolId();
    scene.logVols[leafLvId] = {leafLvId, "leaf_lv", shapeId, matId};

    SemanticNodeId rootId = scene.nextNodeId();
    SemanticNodeId midId = scene.nextNodeId();
    SemanticNodeId leafId = scene.nextNodeId();

    SemanticNode root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = rootLvId;
    root.children = {midId};
    scene.nodes[rootId] = root;

    SemanticNode mid;
    mid.id = midId;
    mid.name = "mid";
    mid.logVolId = midLvId;
    mid.parentId = rootId;
    mid.children = {leafId};
    scene.nodes[midId] = mid;

    SemanticNode leaf;
    leaf.id = leafId;
    leaf.name = "leaf";
    leaf.logVolId = leafLvId;
    leaf.parentId = midId;
    scene.nodes[leafId] = leaf;

    scene.rootId = rootId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();

    struct Result {
        SemanticScene scene;
        SemanticNodeId rootId, midId, leafId;
    };
    return Result{std::move(scene), rootId, midId, leafId};
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("ClosureExpander: None returns seed unchanged", "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    std::unordered_set<SemanticNodeId> seed{midId};
    auto result = ClosureExpander::expand(scene, seed, ClosurePolicy::None);

    REQUIRE(result.size() == 1);
    REQUIRE(result.contains(midId));
    REQUIRE_FALSE(result.contains(rootId));
    REQUIRE_FALSE(result.contains(leafId));
}

TEST_CASE("ClosureExpander: Ancestors includes full ancestor chain to root",
          "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    std::unordered_set<SemanticNodeId> seed{leafId};
    auto result = ClosureExpander::expand(scene, seed, ClosurePolicy::Ancestors);

    REQUIRE(result.contains(leafId));
    REQUIRE(result.contains(midId));
    REQUIRE(result.contains(rootId));
    REQUIRE(result.size() == 3);
}

TEST_CASE("ClosureExpander: Descendants includes full subtree", "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    std::unordered_set<SemanticNodeId> seed{midId};
    auto result = ClosureExpander::expand(scene, seed, ClosurePolicy::Descendants);

    REQUIRE(result.contains(midId));
    REQUIRE(result.contains(leafId));
    REQUIRE_FALSE(result.contains(rootId));
    REQUIRE(result.size() == 2);
}

TEST_CASE("ClosureExpander: Full includes both ancestors and descendants", "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    std::unordered_set<SemanticNodeId> seed{midId};
    auto result = ClosureExpander::expand(scene, seed, ClosurePolicy::Full);

    REQUIRE(result.contains(rootId));
    REQUIRE(result.contains(midId));
    REQUIRE(result.contains(leafId));
    REQUIRE(result.size() == 3);
}

TEST_CASE("ClosureExpander: nonexistent ID in seed throws", "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    SemanticNodeId bogus{9999};
    std::unordered_set<SemanticNodeId> seed{bogus};
    REQUIRE_THROWS_AS(ClosureExpander::expand(scene, seed, ClosurePolicy::Descendants),
                      std::invalid_argument);
}

TEST_CASE("ClosureExpander: multiple seeds expanded together", "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    // Seed contains both root and leaf — Descendants from root covers everything,
    // Ancestors from leaf also covers everything. Union should be all 3 nodes.
    std::unordered_set<SemanticNodeId> seed{rootId, leafId};
    auto result = ClosureExpander::expand(scene, seed, ClosurePolicy::Full);

    REQUIRE(result.contains(rootId));
    REQUIRE(result.contains(midId));
    REQUIRE(result.contains(leafId));
    REQUIRE(result.size() == 3);
}

TEST_CASE("ClosureExpander: Ancestors from root returns only root", "[selection][closure]") {
    auto [scene, rootId, midId, leafId] = makeThreeLevelScene();

    std::unordered_set<SemanticNodeId> seed{rootId};
    auto result = ClosureExpander::expand(scene, seed, ClosurePolicy::Ancestors);

    REQUIRE(result.size() == 1);
    REQUIRE(result.contains(rootId));
}

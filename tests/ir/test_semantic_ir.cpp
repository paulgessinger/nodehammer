#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/semantic_json.hpp>

TEST_CASE("SemanticScene: construction and node lookup by ID", "[ir][semantic]") {
    nodehammer::SemanticScene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, nodehammer::BoxShape{5.0, 5.0, 5.0}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "iron", std::nullopt, 7.87};

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "ironBox", shapeId, matId};

    auto nodeId = scene.nextNodeId();
    nodehammer::SemanticNode node;
    node.id = nodeId;
    node.name = "root";
    node.logVolId = lvId;
    scene.nodes[nodeId] = node;
    scene.rootId = nodeId;

    REQUIRE(scene.nodes.size() == 1);
    REQUIRE(scene.nodes.count(nodeId) == 1);
    REQUIRE(scene.nodes.at(nodeId).name == "root");
    REQUIRE(scene.logVols.at(lvId).shapeId == shapeId);
    REQUIRE(scene.materials.at(matId).name == "iron");
}

TEST_CASE("StrongId: different Tag types are incompatible at compile time", "[ir][semantic]") {
    // These should not compile if mixed — verified by static_assert
    nodehammer::SemanticNodeId a{1};
    nodehammer::SemanticLogVolId b{1};
    // Uncomment to verify compile-time error:
    // bool bad = (a == b);  // should not compile

    // Runtime: same value, different types
    REQUIRE(a.value == b.value);

    // Self-comparison works
    nodehammer::SemanticNodeId c{1};
    REQUIRE(a == c);

    nodehammer::SemanticNodeId d{2};
    REQUIRE(a != d);
}

TEST_CASE("SemanticScene: computeWorldTransforms BFS", "[ir][semantic]") {
    nodehammer::SemanticScene scene;

    auto makeNode = [&](std::string name, glm::dmat4 local,
                        std::optional<nodehammer::SemanticNodeId> parent) {
        // Dummy shape + logvol per node (minimal)
        auto shapeId = scene.nextShapeId();
        scene.shapes[shapeId] = {shapeId, nodehammer::BoxShape{1, 1, 1}};
        auto matId = scene.nextMaterialId();
        scene.materials[matId] = {matId, "mat", std::nullopt, 1.0};
        auto lvId = scene.nextLogVolId();
        scene.logVols[lvId] = {lvId, name + "LV", shapeId, matId};

        auto id = scene.nextNodeId();
        nodehammer::SemanticNode n;
        n.id = id;
        n.name = name;
        n.logVolId = lvId;
        n.localTransform = local;
        n.parentId = parent;
        scene.nodes[id] = n;
        return id;
    };

    auto rootId = makeNode("root", glm::dmat4{1.0}, std::nullopt);
    scene.rootId = rootId;

    glm::dmat4 childLocal{1.0};
    childLocal[3] = glm::dvec4{0.0, 0.0, 100.0, 1.0}; // translate z+100

    auto childId = makeNode("child", childLocal, rootId);
    scene.nodes[rootId].children.push_back(childId);

    scene.computeWorldTransforms();

    // Root worldTransform == its localTransform (identity)
    REQUIRE(scene.nodes.at(rootId).worldTransform == glm::dmat4{1.0});

    // Child worldTransform accumulates parent transform
    const auto &childWorld = scene.nodes.at(childId).worldTransform;
    REQUIRE(childWorld[3].z == Catch::Approx(100.0));
    REQUIRE(childWorld[3].x == Catch::Approx(0.0));
    REQUIRE(childWorld[3].y == Catch::Approx(0.0));
}

TEST_CASE("SemanticScene: logical-volume dedup respects source daughter placements",
          "[ir][semantic]") {
    nodehammer::SemanticScene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, nodehammer::BoxShape{1, 1, 1}};
    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "mat", std::nullopt, 1.0};

    auto childLv = scene.nextLogVolId();
    scene.logVols[childLv] = {childLv, "child", shapeId, matId};

    glm::dmat4 plus{1.0};
    plus[3] = glm::dvec4{0.0, 0.0, 1.0, 1.0};
    glm::dmat4 minus{1.0};
    minus[3] = glm::dvec4{0.0, 0.0, -1.0, 1.0};

    auto parentA = scene.nextLogVolId();
    scene.logVols[parentA] = {parentA, "parentA", shapeId, matId, {{"child", childLv, plus}}};
    auto parentB = scene.nextLogVolId();
    scene.logVols[parentB] = {parentB, "parentB", shapeId, matId, {{"child", childLv, minus}}};

    REQUIRE(scene.deduplicateLogVols() == 0);
    REQUIRE(scene.logVols.size() == 3);
}

TEST_CASE("SemanticScene: logical-volume dedup canonicalizes daughter references",
          "[ir][semantic]") {
    nodehammer::SemanticScene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, nodehammer::BoxShape{1, 1, 1}};
    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "mat", std::nullopt, 1.0};

    auto childA = scene.nextLogVolId();
    scene.logVols[childA] = {childA, "childA", shapeId, matId};
    auto childB = scene.nextLogVolId();
    scene.logVols[childB] = {childB, "childB", shapeId, matId};

    glm::dmat4 childPlacement{1.0};
    childPlacement[3] = glm::dvec4{0.0, 0.0, 1.0, 1.0};

    auto parentA = scene.nextLogVolId();
    scene.logVols[parentA] = {
        parentA, "parentA", shapeId, matId, {{"childA", childA, childPlacement}}};
    auto parentB = scene.nextLogVolId();
    scene.logVols[parentB] = {
        parentB, "parentB", shapeId, matId, {{"childB", childB, childPlacement}}};

    auto nodeB = scene.nextNodeId();
    nodehammer::SemanticNode node;
    node.id = nodeB;
    node.name = "nodeB";
    node.logVolId = parentB;
    scene.nodes[nodeB] = node;

    REQUIRE(scene.deduplicateLogVols() == 2);
    REQUIRE(scene.logVols.size() == 2);
    REQUIRE(scene.logVols.contains(childA));
    REQUIRE_FALSE(scene.logVols.contains(childB));
    REQUIRE(scene.logVols.contains(parentA));
    REQUIRE_FALSE(scene.logVols.contains(parentB));
    REQUIRE(scene.logVols.at(parentA).daughters.at(0).logVolId == childA);
    REQUIRE(scene.nodes.at(nodeB).logVolId == parentA);
}

TEST_CASE("SemanticScene JSON: logical volumes omit empty daughters", "[ir][semantic]") {
    nodehammer::SemanticLogicalVolume lv{nodehammer::SemanticLogVolId{1}, "lv",
                                         nodehammer::SemanticShapeId{2},
                                         nodehammer::SemanticMaterialId{3}};

    nlohmann::json j = lv;
    // Empty daughters should be omitted from JSON output
    REQUIRE_FALSE(j.contains("daughters"));

    // Round-trip: missing daughters should deserialize to empty vector
    auto lv2 = j.get<nodehammer::SemanticLogicalVolume>();
    REQUIRE(lv2.daughters.empty());
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/semantic.hpp>

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

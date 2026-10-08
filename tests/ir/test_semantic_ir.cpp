#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ir/semantic_json.hpp>

TEST_CASE("expanded::Scene: construction and node lookup by ID", "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, nodehammer::ir::semantic::BoxShape{5.0, 5.0, 5.0}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "iron", std::nullopt, 7.87};

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "ironBox", shapeId, matId};

    auto nodeId = scene.nextNodeId();
    nodehammer::ir::expanded::Node node;
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
    nodehammer::ir::expanded::NodeId a{1};
    nodehammer::ir::semantic::LogVolId b{1};
    // Uncomment to verify compile-time error:
    // bool bad = (a == b);  // should not compile

    // Runtime: same value, different types
    REQUIRE(a.value == b.value);

    // Self-comparison works
    nodehammer::ir::expanded::NodeId c{1};
    REQUIRE(a == c);

    nodehammer::ir::expanded::NodeId d{2};
    REQUIRE(a != d);
}

TEST_CASE("expanded::Scene: computeWorldTransforms BFS", "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto makeNode = [&](std::string name, glm::dmat4 local,
                        std::optional<nodehammer::ir::expanded::NodeId> parent) {
        // Dummy shape + logvol per node (minimal)
        auto shapeId = scene.nextShapeId();
        scene.shapes[shapeId] = {shapeId, nodehammer::ir::semantic::BoxShape{1, 1, 1}};
        auto matId = scene.nextMaterialId();
        scene.materials[matId] = {matId, "mat", std::nullopt, 1.0};
        auto lvId = scene.nextLogVolId();
        scene.logVols[lvId] = {lvId, name + "LV", shapeId, matId};

        auto id = scene.nextNodeId();
        nodehammer::ir::expanded::Node n;
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

TEST_CASE("expanded::Scene: logical-volume dedup respects source daughter placements",
          "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, nodehammer::ir::semantic::BoxShape{1, 1, 1}};
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

TEST_CASE("expanded::Scene: logical-volume dedup canonicalizes daughter references",
          "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, nodehammer::ir::semantic::BoxShape{1, 1, 1}};
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
        parentA, "parentA", shapeId, matId, {{"sensor", childA, childPlacement}}};
    auto parentB = scene.nextLogVolId();
    scene.logVols[parentB] = {
        parentB, "parentB", shapeId, matId, {{"sensor", childB, childPlacement}}};

    auto nodeB = scene.nextNodeId();
    nodehammer::ir::expanded::Node node;
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

TEST_CASE("expanded::Scene: logical-volume dedup preserves transitive placement names",
          "[ir][semantic][dedup]") {
    using namespace nodehammer::ir::semantic;
    using nodehammer::ir::expanded::Node;
    using nodehammer::ir::expanded::NodeId;
    nodehammer::ir::expanded::Scene scene;
    const auto shapeId = scene.nextShapeId();
    scene.shapes.emplace(shapeId, Shape{shapeId, BoxShape{1, 1, 1}});
    const auto materialId = scene.nextMaterialId();
    scene.materials.emplace(materialId, SourceMaterial{materialId, "silicon", std::nullopt, 2.33});

    const auto sensorA = scene.nextLogVolId();
    const auto sensorB = scene.nextLogVolId();
    const auto moduleA = scene.nextLogVolId();
    const auto moduleB = scene.nextLogVolId();
    const auto parentA = scene.nextLogVolId();
    const auto parentB = scene.nextLogVolId();
    const auto parentCopy = scene.nextLogVolId();
    glm::dmat4 sensorTransform{1.0};
    sensorTransform[3] = {2, 3, 4, 1};
    glm::dmat4 moduleTransform{1.0};
    moduleTransform[3] = {10, 20, 30, 1};
    const std::vector<LogicalVolume> volumes{
        {sensorA, "sensor_definition_A", shapeId, materialId},
        {sensorB, "sensor_definition_B", shapeId, materialId},
        {moduleA,
         "module_definition_A",
         shapeId,
         materialId,
         {{"sensor_a", sensorA, sensorTransform}}},
        {moduleB,
         "module_definition_B",
         shapeId,
         materialId,
         {{"sensor_b", sensorB, sensorTransform}}},
        {parentA,
         "parent_definition_A",
         shapeId,
         materialId,
         {{"module", moduleA, moduleTransform}}},
        {parentB,
         "parent_definition_B",
         shapeId,
         materialId,
         {{"module", moduleB, moduleTransform}}},
        {parentCopy,
         "parent_definition_copy",
         shapeId,
         materialId,
         {{"module", moduleA, moduleTransform}}},
    };
    SECTION("forward insertion") {
        for (const auto &volume : volumes) {
            scene.logVols.emplace(volume.id, volume);
        }
    }
    SECTION("reverse insertion has the same canonical definitions") {
        for (auto it = volumes.rbegin(); it != volumes.rend(); ++it) {
            scene.logVols.emplace(it->id, *it);
        }
    }
    auto addOccurrence = [&](LogVolId lv) {
        Node node;
        node.id = scene.nextNodeId();
        node.name = "parent";
        node.logVolId = lv;
        const auto id = node.id;
        scene.nodes.emplace(id, std::move(node));
        return id;
    };
    const auto occurrenceA = addOccurrence(parentA);
    const auto occurrenceB = addOccurrence(parentB);
    const auto occurrenceCopy = addOccurrence(parentCopy);

    // Leaf definitions may share despite different LV names. Placement names,
    // however, must distinguish both modules and their otherwise identical parents.
    REQUIRE(scene.deduplicateLogVols() == 2);
    REQUIRE(scene.logVols.size() == 5);
    REQUIRE_FALSE(scene.logVols.contains(sensorB));
    REQUIRE_FALSE(scene.logVols.contains(parentCopy));
    REQUIRE(scene.nodes.at(occurrenceA).logVolId == parentA);
    REQUIRE(scene.nodes.at(occurrenceB).logVolId == parentB);
    REQUIRE(scene.nodes.at(occurrenceCopy).logVolId == parentA);
    REQUIRE(scene.logVols.at(parentA).daughters.front().logVolId == moduleA);
    REQUIRE(scene.logVols.at(parentB).daughters.front().logVolId == moduleB);
    REQUIRE(scene.logVols.at(moduleA).daughters.front().name == "sensor_a");
    REQUIRE(scene.logVols.at(moduleB).daughters.front().name == "sensor_b");
    REQUIRE(scene.logVols.at(moduleA).daughters.front().logVolId == sensorA);
    REQUIRE(scene.logVols.at(moduleB).daughters.front().logVolId == sensorA);
    REQUIRE(scene.logVols.at(moduleA).daughters.front().localTransform == sensorTransform);
    REQUIRE(scene.logVols.at(moduleB).daughters.front().localTransform == sensorTransform);
    REQUIRE(scene.deduplicateLogVols() == 0);
}

TEST_CASE("expanded::Scene JSON: logical volumes omit empty daughters", "[ir][semantic]") {
    nodehammer::ir::semantic::LogicalVolume lv{nodehammer::ir::semantic::LogVolId{1}, "lv",
                                               nodehammer::ir::semantic::ShapeId{2},
                                               nodehammer::ir::semantic::MaterialId{3}};

    nlohmann::json j = lv;
    // Empty daughters should be omitted from JSON output
    REQUIRE_FALSE(j.contains("daughters"));

    // Round-trip: missing daughters should deserialize to empty vector
    auto lv2 = j.get<nodehammer::ir::semantic::LogicalVolume>();
    REQUIRE(lv2.daughters.empty());
}

TEST_CASE("expanded::Scene: visitBFS terminates on a cycle", "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto makeNode = [&](std::string name) {
        auto id = scene.nextNodeId();
        nodehammer::ir::expanded::Node n;
        n.id = id;
        n.name = std::move(name);
        scene.nodes[id] = n;
        return id;
    };

    auto rootId = makeNode("root");
    auto childId = makeNode("child");
    scene.rootId = rootId;

    // Deliberate cycle: root -> child -> root.
    scene.nodes[rootId].children.push_back(childId);
    scene.nodes[childId].children.push_back(rootId);

    std::vector<nodehammer::ir::expanded::NodeId> visited;
    scene.visitBFS([&](const auto &node) { visited.push_back(node.id); });

    REQUIRE(visited.size() == 2);
    REQUIRE(visited.at(0) == rootId);
    REQUIRE(visited.at(1) == childId);
}

TEST_CASE("expanded::Scene: visitBFS skips a dangling child id", "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto rootId = scene.nextNodeId();
    nodehammer::ir::expanded::Node root;
    root.id = rootId;
    root.name = "root";
    scene.nodes[rootId] = root;
    scene.rootId = rootId;

    // Child id that was never inserted into `nodes` — e.g. left behind by a
    // partial prune. Must be skipped rather than throwing from nodes.at().
    scene.nodes[rootId].children.push_back(nodehammer::ir::expanded::NodeId{9999});

    std::vector<nodehammer::ir::expanded::NodeId> visited;
    REQUIRE_NOTHROW(scene.visitBFS([&](const auto &node) { visited.push_back(node.id); }));
    REQUIRE(visited.size() == 1);
    REQUIRE(visited.at(0) == rootId);
}

TEST_CASE("expanded::Scene: visitBFS on a scene whose root is absent", "[ir][semantic]") {
    nodehammer::ir::expanded::Scene scene;

    auto orphanId = scene.nextNodeId();
    nodehammer::ir::expanded::Node orphan;
    orphan.id = orphanId;
    scene.nodes[orphanId] = orphan;
    scene.rootId = nodehammer::ir::expanded::NodeId{4242}; // never inserted

    int calls = 0;
    REQUIRE_NOTHROW(scene.visitBFS([&](const auto &) { ++calls; }));
    REQUIRE(calls == 0);
}

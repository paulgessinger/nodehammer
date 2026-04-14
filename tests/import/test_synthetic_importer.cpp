#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/provenance.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <unordered_set>

// ── SyntheticSceneBuilder: buildSingleBox ─────────────────────────────────────

TEST_CASE("SyntheticSceneBuilder: buildSingleBox — geometry correct", "[import][synthetic]") {
    auto scene = nodehammer::SyntheticSceneBuilder::buildSingleBox();

    REQUIRE(scene.nodes.size() == 1);
    REQUIRE(scene.shapes.size() == 1);
    REQUIRE(scene.materials.size() == 1);

    const auto &rootNode = scene.nodes.at(scene.rootId);
    REQUIRE(rootNode.name == "world");
    REQUIRE(rootNode.sourceSystem == "synthetic");

    const auto &lv = scene.logVols.at(rootNode.logVolId);
    const auto &shape = scene.shapes.at(lv.shapeId);
    REQUIRE(std::holds_alternative<nodehammer::BoxShape>(shape.data));
    REQUIRE(std::get<nodehammer::BoxShape>(shape.data).dx == Catch::Approx(10.0));

    const auto &mat = scene.materials.at(lv.materialId);
    REQUIRE(mat.name == "aluminum");
}

// ── SyntheticSceneBuilder: buildNestedBoxes ───────────────────────────────────

TEST_CASE("SyntheticSceneBuilder: buildNestedBoxes — correct parent-child links",
          "[import][synthetic]") {
    auto scene = nodehammer::SyntheticSceneBuilder::buildNestedBoxes();

    REQUIRE(scene.nodes.size() == 2);

    const auto &root = scene.nodes.at(scene.rootId);
    REQUIRE(root.children.size() == 1);

    const auto childId = root.children.front();
    const auto &child = scene.nodes.at(childId);
    REQUIRE(child.parentId == scene.rootId);
}

TEST_CASE("SyntheticSceneBuilder: buildNestedBoxes — all nodes reachable from root",
          "[import][synthetic]") {
    auto scene = nodehammer::SyntheticSceneBuilder::buildNestedBoxes();

    std::unordered_set<nodehammer::SemanticNodeId> visited;
    scene.visitBFS([&](const auto &node) { visited.insert(node.id); });
    REQUIRE(visited.size() == scene.nodes.size());
}

// ── SyntheticSceneBuilder: buildWithDiagnostics ───────────────────────────────

TEST_CASE("SyntheticSceneBuilder: buildWithDiagnostics — UnknownShape degradation flag set",
          "[import][synthetic]") {
    auto result = nodehammer::SyntheticSceneBuilder::buildWithDiagnostics();

    REQUIRE_FALSE(result.diags.empty());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == nodehammer::codes::kWarnImportUnknownShape) {
            found = true;
        }
    }
    REQUIRE(found);

    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(root.degradation.has(nodehammer::DegradationBit::UnknownShape));
}

// ── SyntheticSceneBuilder: buildBooleanSubtraction ───────────────────────────

TEST_CASE("SyntheticSceneBuilder: buildBooleanSubtraction — boolean shape present",
          "[import][synthetic]") {
    auto scene = nodehammer::SyntheticSceneBuilder::buildBooleanSubtraction();

    bool hasBool = false;
    for (const auto &[id, shape] : scene.shapes) {
        if (std::holds_alternative<nodehammer::BooleanSubtraction>(shape.data)) {
            hasBool = true;
            const auto &bs = std::get<nodehammer::BooleanSubtraction>(shape.data);
            REQUIRE(scene.shapes.contains(bs.left));
            REQUIRE(scene.shapes.contains(bs.right));
        }
    }
    REQUIRE(hasBool);
}

// ── SyntheticImporter: ISemanticImporter contract ────────────────────────────────────

TEST_CASE("SyntheticImporter: formatName and supportedExtensions", "[import][synthetic]") {
    nodehammer::SyntheticImporter imp;
    REQUIRE(imp.formatName() == "synthetic");
    REQUIRE(imp.supportedExtensions().empty());
}

TEST_CASE("SyntheticImporter: import produces no errors", "[import][synthetic]") {
    nodehammer::SyntheticImporter imp;
    auto result = imp.import({});
    REQUIRE_FALSE(result.diags.hasErrors());
}

TEST_CASE("SyntheticImporter: sourceSystem is synthetic", "[import][synthetic]") {
    nodehammer::SyntheticImporter imp;
    auto result = imp.import({});
    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(root.sourceSystem == "synthetic");
}

// ── worldTransform accumulation ───────────────────────────────────────────────

TEST_CASE("SyntheticSceneBuilder: buildNestedBoxes — root has identity worldTransform",
          "[import][synthetic]") {
    auto scene = nodehammer::SyntheticSceneBuilder::buildNestedBoxes();

    const auto &root = scene.nodes.at(scene.rootId);
    const glm::dmat4 identity{1.0};
    REQUIRE(root.worldTransform == identity);
}

TEST_CASE("SyntheticSceneBuilder: buildNestedBoxes — child worldTransform z == 100",
          "[import][synthetic]") {
    auto scene = nodehammer::SyntheticSceneBuilder::buildNestedBoxes();

    const auto &root = scene.nodes.at(scene.rootId);
    const auto childId = root.children.front();
    const auto &child = scene.nodes.at(childId);

    REQUIRE(child.worldTransform[3].x == Catch::Approx(0.0));
    REQUIRE(child.worldTransform[3].y == Catch::Approx(0.0));
    REQUIRE(child.worldTransform[3].z == Catch::Approx(100.0));
}

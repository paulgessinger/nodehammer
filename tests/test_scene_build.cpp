#include <catch2/catch_test_macros.hpp>
#include <ir/expanded/adapt.hpp>
#include <ir/expanded/conversion.hpp>

#include <ir/semantic/occurrence_tree.hpp>
#include <scene_build.hpp>
#include <selection/occurrence_selector.hpp>
#include <tessellation/tessellation_pass.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <variant>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::pipeline;
using namespace nodehammer::tessellation;
using namespace nodehammer::config;

namespace {

// A minimal scene: a root box at the origin plus one wide box straddling the
// +x axis, so a [0°,90°] wedge cut turns it into a Boolean subtraction.
ir::expanded::Scene makeStraddlingScene() {
    ir::expanded::Scene scene;
    const ir::semantic::MaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};

    const ir::semantic::ShapeId rootShape = scene.nextShapeId();
    scene.shapes[rootShape] = {rootShape, ir::semantic::BoxShape{20, 20, 20}};
    const ir::semantic::ShapeId boxShape = scene.nextShapeId();
    scene.shapes[boxShape] = {boxShape, ir::semantic::BoxShape{2, 1, 1}};

    const ir::semantic::LogVolId rootLv = scene.nextLogVolId();
    scene.logVols[rootLv] = {rootLv, "root_lv", rootShape, mat};
    const ir::semantic::LogVolId boxLv = scene.nextLogVolId();
    scene.logVols[boxLv] = {boxLv, "box_lv", boxShape, mat};

    const ir::expanded::NodeId root = scene.nextNodeId();
    ir::expanded::Node rootNode;
    rootNode.id = root;
    rootNode.name = "root";
    rootNode.logVolId = rootLv;
    scene.nodes[root] = rootNode;
    scene.rootId = root;

    const ir::expanded::NodeId box = scene.nextNodeId();
    ir::expanded::Node boxNode;
    boxNode.id = box;
    boxNode.name = "straddle";
    boxNode.logVolId = boxLv;
    boxNode.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{10, 0, 0});
    boxNode.parentId = root;
    scene.nodes[box] = boxNode;
    scene.nodes[root].children = {box};

    scene.computeWorldTransforms();
    return scene;
}

bool hasBooleanSubtraction(const ir::expanded::Scene &scene) {
    for (const auto &[id, shape] : scene.shapes) {
        (void)id;
        if (std::holds_alternative<ir::semantic::BooleanSubtraction>(shape.data)) {
            return true;
        }
    }
    return false;
}

NHConfig noDedupConfig() {
    NHConfig cfg;
    cfg.deduplicateShapes = false;
    return cfg;
}

} // namespace

TEST_CASE("prepareScene: no wedge param leaves geometry uncut", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(
        noDedupConfig(), nodehammer::ir::semantic::fromExpanded(makeStraddlingScene()));
    CHECK_FALSE(
        hasBooleanSubtraction(prep.cutScene ? *prep.cutScene : ir::semantic::expand(prep.scene)));
}

TEST_CASE("prepareScene: wedge param applies the Boolean cut", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(
        noDedupConfig(), nodehammer::ir::semantic::fromExpanded(makeStraddlingScene()),
        WedgeCutParams{0.0, 90.0});
    // The straddling box becomes a BooleanSubtraction(box, wedge) shape.
    CHECK(hasBooleanSubtraction(prep.cutScene ? *prep.cutScene : ir::semantic::expand(prep.scene)));
}

TEST_CASE("prepareScene: degenerate wedge is a no-op", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(
        noDedupConfig(), nodehammer::ir::semantic::fromExpanded(makeStraddlingScene()),
        WedgeCutParams{45.0, 45.0});
    CHECK_FALSE(
        hasBooleanSubtraction(prep.cutScene ? *prep.cutScene : ir::semantic::expand(prep.scene)));
}

TEST_CASE("prepareScene: hoisted repeated assemblies retain mesh sharing",
          "[scene_build][selection][hoist]") {
    using namespace ir::semantic;
    Scene scene;
    const MaterialId material{1};
    const ShapeId shape{1};
    scene.materials.emplace(material, SourceMaterial{material, "Silicon", std::nullopt, 2.33});
    scene.shapes.emplace(shape, Shape{shape, BoxShape{1, 2, 3}});
    for (uint64_t i = 1; i <= 5; ++i) {
        scene.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "volume", shape, material});
    }
    scene.root = {"world", LogVolId{1}, glm::dmat4{1.0}};
    const auto gap = glm::rotate(glm::translate(glm::dmat4{1.0}, glm::dvec3{1.3, 2.7, 0.4}), 0.37,
                                 glm::dvec3{0, 0, 1});
    const auto inner = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.2, -0.8, 1.1});
    const auto sensor = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.7, 0.3, 2.9});
    scene.logVols.at(LogVolId{2}).daughters = {{"gap", LogVolId{3}, gap}};
    scene.logVols.at(LogVolId{3}).daughters = {{"gap_inner", LogVolId{4}, inner}};
    scene.logVols.at(LogVolId{4}).daughters = {{"sensor", LogVolId{5}, sensor}};
    for (int i = 0; i < 3; ++i) {
        const auto placement =
            glm::rotate(glm::translate(glm::dmat4{1.0}, glm::dvec3{123.4 * i, -56.7 * i, 9.1 * i}),
                        0.61 * i, glm::dvec3{0, 0, 1});
        scene.logVols.at(LogVolId{1})
            .daughters.push_back({"assembly" + std::to_string(i), LogVolId{2}, placement});
    }
    auto original = scene.makeTree(0);
    NHConfig cfg;
    cfg.hoistOrphans = true;
    SelectionRule drop;
    drop.action = SelectionAction::DropIf;
    drop.predicate = PredicateExpr{NameGlobPredicate{"gap*"}};
    SelectionRule keep;
    keep.action = SelectionAction::KeepIf;
    keep.predicate = PredicateExpr{NameGlobPredicate{"sensor"}};
    cfg.selection = {drop, keep};
    Rule merge;
    merge.match = PredicateExpr{NameGlobPredicate{"assembly*"}};
    merge.tessellation.emplace();
    merge.tessellation->mergeDescendants = true;
    Rule world;
    world.match = PredicateExpr{NameGlobPredicate{"world"}};
    world.tessellation.emplace();
    world.tessellation->skipGeometry = true;
    cfg.rules = {merge, world};
    const auto selected = selection::SelectedOccurrences{*original}.prune(cfg.selection, true);
    const auto prep = prepareSceneForTessellationFromInputs(cfg, scene);
    REQUIRE_FALSE(prep.diags.hasErrors());
    const auto expectedLocal = gap * inner * sensor;
    std::size_t sensors = 0;
    const auto physical = ir::semantic::expand(prep.scene);
    for (const auto &[id, node] : physical.nodes) {
        (void)id;
        if (node.name != "sensor") {
            continue;
        }
        REQUIRE(node.parentId.has_value());
        const auto &parent = physical.nodes.at(*node.parentId);
        const auto index = static_cast<std::size_t>(parent.name.back() - '0');
        const auto expected = original->materialize({index, 0, 0, 0});
        const auto selectedNode = selected.materialize({index, 0, 0, 0});
        CHECK(node.originalPath == expected->originalPath);
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                CHECK(node.localTransform[col][row] == expectedLocal[col][row]);
                CHECK(selectedNode.localTransform[col][row] == expectedLocal[col][row]);
                CHECK(std::abs(node.worldTransform[col][row] - expected->worldTransform[col][row]) <
                      1e-10);
            }
        }
        ++sensors;
    }
    CHECK(sensors == 3);
    const auto rendered = TessellationPass{prep.config}.lower(prep.scene);
    REQUIRE_FALSE(rendered.diags.hasErrors());
    CHECK(rendered.scene.meshAssets.size() == 2); // shared primitive and shared merged mesh
}

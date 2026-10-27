#include <catch2/catch_test_macros.hpp>
#include <config/config_loader.hpp>
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

bool hasBooleanSubtraction(const ir::semantic::GeometryCatalogs &scene) {
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
    CHECK_FALSE(hasBooleanSubtraction(prep.scene));
}

TEST_CASE("prepareScene: wedge param applies the Boolean cut", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(
        noDedupConfig(), nodehammer::ir::semantic::fromExpanded(makeStraddlingScene()),
        WedgeCutParams{0.0, 90.0});
    // The straddling box becomes a BooleanSubtraction(box, wedge) shape.
    CHECK(hasBooleanSubtraction(prep.scene));
}

TEST_CASE("prepareScene: degenerate wedge is a no-op", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(
        noDedupConfig(), nodehammer::ir::semantic::fromExpanded(makeStraddlingScene()),
        WedgeCutParams{45.0, 45.0});
    CHECK_FALSE(hasBooleanSubtraction(prep.scene));
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

TEST_CASE("prepareScene: fitted hull replaces cells before cutting and survives assembly skip",
          "[scene_build][approximation][wedgecut]") {
    using namespace ir::semantic;
    Scene scene;
    const MaterialId material{1};
    scene.materials.emplace(material, SourceMaterial{material, "glass", std::nullopt, 2.5});
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{100, 100, 100}});
    scene.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{1, 1, 1}});
    for (uint64_t i = 1; i <= 3; ++i)
        scene.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "volume",
                                                         ShapeId{i == 3 ? 2ULL : 1ULL}, material});
    scene.root = {"world", LogVolId{1}, glm::dmat4{1}};
    scene.logVols.at(LogVolId{1}).daughters = {
        {"module", LogVolId{2}, glm::translate(glm::dmat4{1}, glm::dvec3{10, 0, 0})}};
    scene.logVols.at(LogVolId{2}).daughters = {
        {"cell0", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{-2, 0, 0})},
        {"cell1", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{2, 0, 0})}};
    scene.reseedIdCounters();
    NHConfig cfg;
    Rule world;
    world.match = PredicateExpr{NameGlobPredicate{"world"}};
    world.tessellation.emplace();
    world.tessellation->skipGeometry = true;
    Rule module;
    module.match = PredicateExpr{NameGlobPredicate{"module"}};
    module.tessellation.emplace();
    module.tessellation->skipGeometry = true;
    module.geometry = Rule::Geometry{"convex_hull", std::nullopt};
    cfg.rules = {world, module};
    std::optional<WedgeCutParams> cut;
    SECTION("uncut generated hull remains visible") {}
    SECTION("Boolean wedge operates on hull instead of cells") { cut = WedgeCutParams{0, 90}; }
    const auto prep = prepareSceneForTessellationFromInputs(cfg, scene, cut);
    REQUIRE_FALSE(prep.diags.hasErrors());
    CHECK(scene.nodeCount() == 4); // Input remains immutable.
    REQUIRE(prep.scene.nodeCount() == 2);
    if (cut) {
        bool cutHull = false;
        for (const auto &[id, shape] : prep.scene.shapes) {
            (void)id;
            if (const auto *subtraction = std::get_if<BooleanSubtraction>(&shape.data))
                cutHull |= std::holds_alternative<TessellatedShape>(
                    prep.scene.shapes.at(subtraction->left).data);
        }
        CHECK(cutHull);
    }
    const auto rendered = TessellationPass{prep.config}.lower(prep.scene, &prep.cutOverrides);
    REQUIRE_FALSE(rendered.diags.hasErrors());
    REQUIRE(rendered.scene.nodes.size() == 2);
    bool visible = false;
    for (const auto &[id, node] : rendered.scene.nodes) {
        (void)id;
        if (node.name == "module") {
            REQUIRE_FALSE(node.meshBindings.empty());
            visible = true;
        }
        CHECK(node.name != "cell0");
        CHECK(node.name != "cell1");
    }
    REQUIRE(visible);
    REQUIRE_FALSE(rendered.scene.meshAssets.empty());
    for (const auto &[id, mesh] : rendered.scene.meshAssets) {
        (void)id;
        REQUIRE_FALSE(mesh.indices.empty());
        for (const auto &vertex : mesh.vertices)
            for (int axis = 0; axis < 3; ++axis)
                CHECK(std::isfinite(vertex.position[axis]));
    }
}

TEST_CASE("prepareScene: drift gas suppression precedes cutting and wire tag rules apply",
          "[scene_build][drift][wedgecut]") {
    using namespace ir::semantic;
    Scene scene;
    scene.materials.emplace(MaterialId{1}, SourceMaterial{MaterialId{1}, "gas", std::nullopt, 0});
    scene.materials.emplace(MaterialId{2},
                            SourceMaterial{MaterialId{2}, "wire_metal", std::nullopt, 0});
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, TubeShape{10, 100, 100}});
    scene.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{2, 1, 50}});
    scene.shapes.emplace(ShapeId{3}, Shape{ShapeId{3}, TubeShape{0, 0.01, 50}});
    scene.logVols.emplace(LogVolId{1},
                          LogicalVolume{LogVolId{1}, "chamber", ShapeId{1}, MaterialId{1}});
    scene.logVols.emplace(LogVolId{2},
                          LogicalVolume{LogVolId{2}, "cell", ShapeId{2}, MaterialId{1}});
    scene.logVols.emplace(LogVolId{3},
                          LogicalVolume{LogVolId{3}, "wire", ShapeId{3}, MaterialId{2}});
    scene.root = {"chamber", LogVolId{1}};
    scene.logVols.at(LogVolId{1}).daughters = {
        {"cell", LogVolId{2}, glm::translate(glm::dmat4{1}, glm::dvec3{20, 0, 0})}};
    // The gas cell straddles the cut boundary; its wire sits outside the wedge.
    scene.logVols.at(LogVolId{2}).daughters = {
        {"wire", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{0, -0.5, 0})}};
    scene.reseedIdCounters();
    auto cfg = ConfigLoader::loadFromString(R"(
[[rules]]
match = 'name == "chamber"'
[rules.geometry.drift_chamber]
gas = 'material == "gas"'
wires = 'material == "wire_metal"'

[[rules]]
match = 'tag.nodehammer_drift_role == "wire"'
[rules.tessellation]
max_segments_circle = 12
)")
                   .config;
    std::optional<WedgeCutParams> cut;
    SECTION("uncut") {}
    SECTION("wedge cuts the envelope but not the suppressed cell") { cut = WedgeCutParams{0, 90}; }
    const auto prep = prepareSceneForTessellationFromInputs(cfg, scene, cut);
    REQUIRE_FALSE(prep.diags.hasErrors());
    REQUIRE(prep.scene.nodeCount() == 3);
    const auto tree = prep.scene.makeTree();
    const auto cell = tree->materialize({0});
    REQUIRE(cell != nullptr);
    const auto cellShape = prep.scene.logVols.at(cell->logVolId).shapeId;
    const auto *empty = std::get_if<TessellatedShape>(&prep.scene.shapes.at(cellShape).data);
    REQUIRE(empty != nullptr);
    CHECK(empty->triangles.empty());
    // A late suppression would leave an unnecessary Boolean for the cell.
    for (const auto &[id, shape] : prep.scene.shapes) {
        (void)id;
        if (const auto *boolean = std::get_if<BooleanSubtraction>(&shape.data)) {
            CHECK(boolean->left != cellShape);
            CHECK_FALSE(std::holds_alternative<BoxShape>(prep.scene.shapes.at(boolean->left).data));
        }
    }
    if (cut)
        CHECK(hasBooleanSubtraction(prep.scene));
    CHECK(std::holds_alternative<BoxShape>(scene.shapes.at(ShapeId{2}).data));
    const auto rendered = TessellationPass{prep.config}.lower(prep.scene, &prep.cutOverrides);
    REQUIRE_FALSE(rendered.diags.hasErrors());
    bool sawWire = false;
    for (const auto &[id, node] : rendered.scene.nodes) {
        (void)id;
        if (node.name == "cell")
            CHECK(node.meshBindings.empty());
        if (node.name != "wire")
            continue;
        sawWire = true;
        REQUIRE(node.meshBindings.size() == 1);
        const auto &mesh = rendered.scene.meshAssets.at(node.meshBindings.front().meshId);
        // Twelve cylinder sectors: two wall triangles and two cap triangles each.
        CHECK(mesh.indices.size() == 12 * 4 * 3);
    }
    CHECK(sawWire);
}

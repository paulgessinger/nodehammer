#include <catch2/catch_test_macros.hpp>

#include <scene_build.hpp>

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
ir::semantic::Scene makeStraddlingScene() {
    ir::semantic::Scene scene;
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

    const ir::semantic::NodeId root = scene.nextNodeId();
    ir::semantic::Node rootNode;
    rootNode.id = root;
    rootNode.name = "root";
    rootNode.logVolId = rootLv;
    scene.nodes[root] = rootNode;
    scene.rootId = root;

    const ir::semantic::NodeId box = scene.nextNodeId();
    ir::semantic::Node boxNode;
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

bool hasBooleanSubtraction(const ir::semantic::Scene &scene) {
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
    auto prep = prepareSceneForTessellationFromInputs(noDedupConfig(), makeStraddlingScene());
    CHECK_FALSE(hasBooleanSubtraction(prep.scene));
}

TEST_CASE("prepareScene: wedge param applies the Boolean cut", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(noDedupConfig(), makeStraddlingScene(),
                                                      WedgeCutParams{0.0, 90.0});
    // The straddling box becomes a BooleanSubtraction(box, wedge) shape.
    CHECK(hasBooleanSubtraction(prep.scene));
}

TEST_CASE("prepareScene: degenerate wedge is a no-op", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(noDedupConfig(), makeStraddlingScene(),
                                                      WedgeCutParams{45.0, 45.0});
    CHECK_FALSE(hasBooleanSubtraction(prep.scene));
}

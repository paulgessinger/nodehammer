#include <catch2/catch_test_macros.hpp>

#include <nodehammer/scene_build.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <variant>

using namespace nodehammer;

namespace {

// A minimal scene: a root box at the origin plus one wide box straddling the
// +x axis, so a [0°,90°] wedge cut turns it into a Boolean subtraction.
detail::SemanticScene makeStraddlingScene() {
    detail::SemanticScene scene;
    const SemanticMaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};

    const SemanticShapeId rootShape = scene.nextShapeId();
    scene.shapes[rootShape] = {rootShape, BoxShape{20, 20, 20}};
    const SemanticShapeId boxShape = scene.nextShapeId();
    scene.shapes[boxShape] = {boxShape, BoxShape{2, 1, 1}};

    const SemanticLogVolId rootLv = scene.nextLogVolId();
    scene.logVols[rootLv] = {rootLv, "root_lv", rootShape, mat};
    const SemanticLogVolId boxLv = scene.nextLogVolId();
    scene.logVols[boxLv] = {boxLv, "box_lv", boxShape, mat};

    const SemanticNodeId root = scene.nextNodeId();
    SemanticNode rootNode;
    rootNode.id = root;
    rootNode.name = "root";
    rootNode.logVolId = rootLv;
    scene.nodes[root] = rootNode;
    scene.rootId = root;

    const SemanticNodeId box = scene.nextNodeId();
    SemanticNode boxNode;
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

bool hasBooleanSubtraction(const detail::SemanticScene &scene) {
    for (const auto &[id, shape] : scene.shapes) {
        (void)id;
        if (std::holds_alternative<BooleanSubtraction>(shape.data)) {
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
    REQUIRE(prep.ok);
    CHECK_FALSE(hasBooleanSubtraction(prep.scene));
}

TEST_CASE("prepareScene: wedge param applies the Boolean cut", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(noDedupConfig(), makeStraddlingScene(),
                                                      WedgeCutParams{0.0, 90.0});
    REQUIRE(prep.ok);
    // The straddling box becomes a BooleanSubtraction(box, wedge) shape.
    CHECK(hasBooleanSubtraction(prep.scene));
}

TEST_CASE("prepareScene: degenerate wedge is a no-op", "[scene_build][wedgecut]") {
    auto prep = prepareSceneForTessellationFromInputs(noDedupConfig(), makeStraddlingScene(),
                                                      WedgeCutParams{45.0, 45.0});
    REQUIRE(prep.ok);
    CHECK_FALSE(hasBooleanSubtraction(prep.scene));
}

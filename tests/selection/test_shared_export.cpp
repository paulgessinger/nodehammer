#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ir/expanded/conversion.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <selection/selector.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::ir::semantic;
using namespace nodehammer::config;

namespace {
semantic::Scene repeatedExportScene(std::size_t instances = 12) {
    semantic::Scene scene;
    scene.sourceFile = "shared-selection-fixture";
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{1, 2, 3}});
    scene.materials.emplace(MaterialId{1}, SourceMaterial{MaterialId{1}, "Silicon", {}, 2.3});
    for (uint64_t id = 1; id <= 4; ++id)
        scene.logVols.emplace(
            LogVolId{id},
            LogicalVolume{LogVolId{id}, "volume" + std::to_string(id), ShapeId{1}, MaterialId{1}});
    scene.root = {"world", LogVolId{1}, glm::dmat4{1}};
    for (std::size_t i = 0; i < instances; ++i)
        scene.logVols.at(LogVolId{1})
            .daughters.push_back({"module" + std::to_string(i), LogVolId{2},
                                  glm::translate(glm::dmat4{1}, glm::dvec3{double(i), 0, 0})});
    scene.logVols.at(LogVolId{2}).daughters = {
        {"fiber", LogVolId{4}, glm::dmat4{1}},
        {"sensor", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{0, 2, 0})},
        {"sensor2", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{0, 3, 0})}};
    scene.metadata.tagSets = {{{"sensitive", "true"}, {"kind", "sensor"}}, {{"readout", "A"}}, {}};
    scene.metadata.volumeTags[LogVolId{3}] = 0;
    scene.metadata.placementTags[{LogVolId{2}, 1}] = 1;
    scene.metadata.defaultSourceSystem = "dd4hep/tgeo";
    scene.reseedIdCounters();
    return scene;
}
template <class Predicate> SelectionRule drop(Predicate predicate) {
    SelectionRule rule;
    rule.action = SelectionAction::DropIf;
    rule.predicate = PredicateExpr{std::move(predicate)};
    return rule;
}
void compareWithExpanded(semantic::Scene &scene, const std::vector<SelectionRule> &rules,
                         bool hoist = false) {
    auto reference = semantic::expand(scene);
    const selection::SelectionEngine engine{rules, hoist};
    auto expectedDiags = engine.prune(reference);
    auto actualDiags = engine.prune(scene);
    REQUIRE(actualDiags.hasErrors() == expectedDiags.hasErrors());
    REQUIRE(scene.validate() == reference.nodes.size());
    auto tree = scene.makeTree(0);
    struct Pair {
        OccurrenceId path;
        expanded::NodeId node;
    };
    std::vector<Pair> pending{{{}, reference.rootId}};
    while (!pending.empty()) {
        auto pair = std::move(pending.back());
        pending.pop_back();
        auto actual = tree->materialize(pair.path);
        const auto &expected = reference.nodes.at(pair.node);
        REQUIRE(actual->name == expected.name);
        REQUIRE(actual->originalPath == expected.originalPath);
        REQUIRE(actual->tags == expected.tags);
        REQUIRE(actual->sourceSystem == expected.sourceSystem);
        REQUIRE(actual->degradation.bits == expected.degradation.bits);
        const auto &av = scene.logVols.at(actual->logVolId);
        const auto &ev = reference.logVols.at(expected.logVolId);
        REQUIRE(av.shapeId == ev.shapeId);
        REQUIRE(av.materialId == ev.materialId);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                REQUIRE(actual->localTransform[c][r] ==
                        Catch::Approx(expected.localTransform[c][r]).margin(1e-12));
                REQUIRE(actual->worldTransform[c][r] ==
                        Catch::Approx(expected.worldTransform[c][r]).margin(1e-12));
            }
        REQUIRE(actual->childCount == expected.children.size());
        for (std::size_t i = 0; i < expected.children.size(); ++i) {
            auto path = pair.path;
            path.push_back(i);
            auto expectedChild = expected.children[i];
            if (hoist) {
                // The expanded oracle appends hoisted children; canonical selection
                // splices them in source order. Compare identity under the same parent.
                const auto originalPath = tree->materialize(path)->originalPath;
                const auto it =
                    std::find_if(expected.children.begin(), expected.children.end(), [&](auto id) {
                        return reference.nodes.at(id).originalPath == originalPath;
                    });
                REQUIRE(it != expected.children.end());
                expectedChild = *it;
            }
            pending.push_back({std::move(path), expectedChild});
        }
    }
}
} // namespace

TEST_CASE("Selection export reuses filtered modules and retains metadata defaults",
          "[selection][shared-export]") {
    auto scene = repeatedExportScene(128);
    compareWithExpanded(scene, {drop(NameGlobPredicate{"fiber"})});
    REQUIRE(scene.logVols.size() == 3);
    REQUIRE(scene.metadata.originalPaths.empty());
    REQUIRE(scene.metadata.sourceSystems.empty());
    REQUIRE(scene.metadata.occurrenceTags.empty());
    REQUIRE(scene.metadata.volumeTags.size() == 1);
    REQUIRE(scene.metadata.placementTags.size() == 1);
    const auto &placements = scene.logVols.at(scene.root.logVolId).daughters;
    for (const auto &placement : placements)
        REQUIRE(placement.logVolId == placements.front().logVolId);
    auto restored = sceneFromBytes(sceneToBytes(scene));
    REQUIRE(restored.validate() == scene.validate());
    REQUIRE(restored.metadata.originalPaths.empty());
    REQUIRE(restored.logVols.size() == 3);
}

TEST_CASE("Selection export remaps sparse overrides without duplicating geometry",
          "[selection][shared-export]") {
    auto scene = repeatedExportScene();
    scene.metadata.occurrenceTags[{1, 1}] = 2; // Explicitly empty, overriding both defaults.
    scene.metadata.displayNames[{2, 1}] = "readout display";
    scene.metadata.pathNames[{2}] = "importedModule";
    scene.metadata.pathNames[{2, 1}] = "importedSensor";
    scene.metadata.originalPaths[{3, 1}] = "/historical/sensor";
    scene.metadata.sourceSystems[{4, 1}] = ""; // Explicitly clears nonempty default.
    scene.metadata.degradation[{5, 1}].set(DegradationBit::TransformApprox);
    compareWithExpanded(scene, {drop(NameGlobPredicate{"fiber"})});
    REQUIRE(scene.logVols.size() == 3);
    REQUIRE(scene.metadata.originalPaths.size() == 1);
    REQUIRE(scene.metadata.sourceSystems.size() == 1);
    REQUIRE(scene.metadata.occurrenceTags.size() == 1);
    auto tree = scene.makeTree(0);
    REQUIRE(tree->materialize({1, 0})->tags.empty());
    REQUIRE(tree->materialize({4, 0})->sourceSystem.empty());
    compareWithExpanded(scene, {drop(PathGlobPredicate{"/world/importedModule/importedSensor"})});
}

TEST_CASE("Selection export creates variants only where occurrence selections differ",
          "[selection][shared-export]") {
    auto scene = repeatedExportScene();
    compareWithExpanded(scene, {drop(NameGlobPredicate{"fiber"}),
                                drop(PathGlobPredicate{"/world/module0/sensor"})});
    REQUIRE(scene.logVols.size() == 4);
    const auto &placements = scene.logVols.at(scene.root.logVolId).daughters;
    REQUIRE(placements[0].logVolId != placements[1].logVolId);
    for (std::size_t i = 2; i < placements.size(); ++i)
        REQUIRE(placements[i].logVolId == placements[1].logVolId);
    REQUIRE(scene.metadata.originalPaths.empty());
    REQUIRE(scene.metadata.sourceSystems.empty());
    REQUIRE(scene.metadata.occurrenceTags.empty());
}

TEST_CASE("Selection export preserves hoisted paths transforms and placement tags",
          "[selection][shared-export]") {
    auto scene = repeatedExportScene(3);
    scene.logVols.at(LogVolId{1}).daughters[0].localTransform =
        glm::rotate(glm::dmat4{1}, .7, glm::dvec3{0, 0, 1});
    scene.metadata.pathNames[{0}] = "originalModule";
    scene.metadata.occurrenceTags[{0, 2}] = 2;
    compareWithExpanded(
        scene, {drop(NameGlobPredicate{"module0"}), drop(NameGlobPredicate{"fiber"})}, true);
    compareWithExpanded(scene, {drop(PathGlobPredicate{"/world/originalModule/sensor"})});
}

TEST_CASE("Selection export keeps original descendant paths across a hoisted subtree",
          "[selection][shared-export]") {
    auto scene = repeatedExportScene(3);
    scene.logVols.emplace(LogVolId{5},
                          LogicalVolume{LogVolId{5}, "leaf", ShapeId{1}, MaterialId{1}});
    scene.logVols.at(LogVolId{3}).daughters = {
        {"leaf", LogVolId{5}, glm::translate(glm::dmat4{1}, glm::dvec3{0, 0, 4})}};
    scene.metadata.pathNames[{0}] = "importedModule";
    scene.metadata.pathNames[{0, 1}] = "importedSensor";
    scene.metadata.displayNames[{0, 1}] = "SensorDisplay";
    scene.metadata.originalPaths[{0, 1}] = "/historical/sensor";
    scene.metadata.pathNames[{0, 1, 0}] = "importedLeaf";
    scene.reseedIdCounters();
    compareWithExpanded(
        scene, {drop(NameGlobPredicate{"module0"}), drop(NameGlobPredicate{"fiber"})}, true);
    REQUIRE(scene.metadata.originalPaths.size() <= 3);
    compareWithExpanded(
        scene, {drop(PathGlobPredicate{"/world/importedModule/importedSensor/importedLeaf"})});
}

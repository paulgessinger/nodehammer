#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <config/predicate_parser.hpp>
#include <geometry/drift_chamber.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <selection/predicate.hpp>

using namespace nodehammer;
using namespace nodehammer::ir::semantic;
namespace {
bool warnings(const DiagnosticList &diags) {
    return std::any_of(diags.begin(), diags.end(),
                       [](const auto &d) { return d.severity == Diagnostic::Severity::Warning; });
}
Scene chamberScene() {
    Scene s;
    for (const auto &[id, name] :
         std::vector<std::pair<int, std::string>>{{1, "gas"}, {2, "metal"}})
        s.materials.emplace(
            MaterialId{static_cast<uint64_t>(id)},
            SourceMaterial{MaterialId{static_cast<uint64_t>(id)}, name, std::nullopt, 0});
    s.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, TubeShape{10, 100, 100}});
    s.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{1, 1, 50}});
    s.shapes.emplace(ShapeId{3}, Shape{ShapeId{3}, TubeShape{0, 0.01, 50}});
    s.shapes.emplace(ShapeId{4}, Shape{ShapeId{4}, BoxShape{120, 120, 120}});
    s.logVols.emplace(LogVolId{1}, LogicalVolume{LogVolId{1}, "world", ShapeId{4}, MaterialId{2}});
    s.logVols.emplace(LogVolId{2},
                      LogicalVolume{LogVolId{2}, "chamber", ShapeId{1}, MaterialId{1}});
    s.logVols.emplace(LogVolId{3}, LogicalVolume{LogVolId{3}, "cell", ShapeId{2}, MaterialId{1}});
    s.logVols.emplace(LogVolId{4}, LogicalVolume{LogVolId{4}, "wire", ShapeId{3}, MaterialId{2}});
    s.logVols.at(LogVolId{1}).daughters = {{"A", LogVolId{2}}, {"B", LogVolId{2}}};
    s.logVols.at(LogVolId{2}).daughters = {{"c0", LogVolId{3}}, {"c1", LogVolId{3}}};
    s.logVols.at(LogVolId{3}).daughters = {
        {"wire", LogVolId{4}, glm::translate(glm::dmat4{1}, glm::dvec3{20, 0, 0})}};
    s.root = {"world", LogVolId{1}};
    s.reseedIdCounters();
    return s;
}
config::NHConfig configFor(std::string match = "name ~= \"A\"") {
    config::NHConfig cfg;
    config::Rule r;
    r.match = config::parsePredicateExpr(match).value();
    r.geometry.emplace();
    r.geometry->driftChamber.emplace();
    r.geometry->driftChamber->gas = "material ~= \"gas\"";
    r.geometry->driftChamber->wires = "name ~= \"wire\"";
    cfg.rules.push_back(r);
    return cfg;
}
const ShapeVariant &shape(const Scene &scene, OccurrenceId route) {
    return scene.shapes.at(scene.logVols.at(scene.makeTree()->materialize(route)->logVolId).shapeId)
        .data;
}
} // namespace
TEST_CASE("Drift chamber preserves wires and isolates shared occurrence gas surfaces",
          "[geometry][drift]") {
    auto scene = chamberScene();
    const auto original = scene;
    scene.metadata.tagSets.push_back({{"subsystem", "tracker"}});
    scene.metadata.occurrenceTags[{0, 0, 0}] = 0;
    auto diags = geometry::optimizeDriftChambers(scene, configFor());
    REQUIRE_FALSE(diags.hasErrors());
    CHECK(scene.nodeCount() == original.nodeCount());
    CHECK(std::holds_alternative<TubeShape>(shape(scene, {0})));
    CHECK(std::get<TessellatedShape>(shape(scene, {0, 0})).triangles.empty());
    CHECK(std::holds_alternative<BoxShape>(shape(scene, {1, 0})));
    auto tree = scene.makeTree();
    const auto w = tree->materialize({0, 0, 0});
    CHECK(w->worldTransform == original.makeTree()->materialize({0, 0, 0})->worldTransform);
    CHECK(w->tags.at("subsystem") == "tracker");
    CHECK(w->tags.at(geometry::kDriftRoleTag) == "wire");
    CHECK(w->logVolId == tree->materialize({0, 1, 0})->logVolId);
    CHECK_FALSE(tree->materialize({1, 0, 0})->tags.contains(geometry::kDriftRoleTag));
    auto expr = config::parsePredicateExpr("tag.nodehammer_drift_role == \"wire\"");
    REQUIRE(expr.has_value());
    CHECK(selection::compilePredicate(*expr)(
        {w->name, w->originalPath, w->materialName, true, &w->tags}));
    const auto bytes = sceneToBytes(scene);
    auto restored = sceneFromBytes(bytes);
    REQUIRE_FALSE(geometry::optimizeDriftChambers(restored, configFor()).hasErrors());
    CHECK(sceneToBytes(restored) == bytes);
}
TEST_CASE("Drift chamber retains unsupported wires and gas with no enclosing matched solid",
          "[geometry][drift]") {
    auto scene = chamberScene();
    scene.logVols.at(LogVolId{2}).materialId = MaterialId{2};
    scene.shapes.at(ShapeId{3}).data = TubeShape{0.001, 0.01, 50};
    auto diags = geometry::optimizeDriftChambers(scene, configFor());
    CHECK_FALSE(diags.hasErrors());
    CHECK(warnings(diags));
    CHECK(std::holds_alternative<BoxShape>(shape(scene, {0, 0})));
    CHECK_FALSE(scene.makeTree()->materialize({0, 0, 0})->tags.contains(geometry::kDriftRoleTag));
}
TEST_CASE("Drift chamber auto wire detection does not guess gas materials", "[geometry][drift]") {
    auto scene = chamberScene();
    auto cfg = configFor();
    cfg.rules[0].geometry->driftChamber->gas.reset();
    cfg.rules[0].geometry->driftChamber->wires.reset();
    auto diags = geometry::optimizeDriftChambers(scene, cfg);
    CHECK(warnings(diags));
    CHECK(std::holds_alternative<BoxShape>(shape(scene, {0, 0})));
    CHECK(scene.makeTree()->materialize({0, 0, 0})->tags.at(geometry::kDriftRoleTag) == "wire");
}
TEST_CASE("Drift chamber leaves overlapping gas and wire candidates unchanged",
          "[geometry][drift]") {
    auto scene = chamberScene();
    auto cfg = configFor();
    cfg.rules[0].geometry->driftChamber->gas = "true";
    cfg.rules[0].geometry->driftChamber->wires = "true";
    const auto diags = geometry::optimizeDriftChambers(scene, cfg);
    CHECK(warnings(diags));
    CHECK(std::holds_alternative<BoxShape>(shape(scene, {0, 0})));
    CHECK_FALSE(scene.makeTree()->materialize({0, 0})->tags.contains(geometry::kDriftRoleTag));
}

TEST_CASE("Baked drift chamber warns when optimization selectors change", "[geometry][drift]") {
    auto scene = chamberScene();
    auto cfg = configFor();
    REQUIRE_FALSE(geometry::optimizeDriftChambers(scene, cfg).hasErrors());
    const auto bytes = sceneToBytes(scene);
    cfg.rules[0].geometry->driftChamber->gas = "false";
    CHECK(warnings(geometry::optimizeDriftChambers(scene, cfg)));
    CHECK(sceneToBytes(scene) == bytes);
}
TEST_CASE("Drift chamber root matching honors persisted path names", "[geometry][drift]") {
    auto scene = chamberScene();
    scene.metadata.pathNames[{}] = "OriginalWorld";
    auto cfg = configFor("path ~= \"/OriginalWorld/A\"");
    REQUIRE_FALSE(geometry::optimizeDriftChambers(scene, cfg).hasErrors());
    CHECK(std::get<TessellatedShape>(shape(scene, {0, 0})).triangles.empty());
}
TEST_CASE("Drift chamber diagnoses unmatched explicit component predicates", "[geometry][drift]") {
    auto scene = chamberScene();
    auto cfg = configFor();
    cfg.rules[0].geometry->driftChamber->wires = "name ~= \"missing\"";
    const auto diags = geometry::optimizeDriftChambers(scene, cfg);
    CHECK(warnings(diags));
    CHECK(std::any_of(diags.begin(), diags.end(), [](const auto &d) {
        return d.message == "wire predicate matched no nodes";
    }));
}
TEST_CASE("Shared chamber roots retain distinct optimization policies", "[geometry][drift]") {
    auto scene = chamberScene();
    auto cfg = configFor();
    auto second = configFor("name ~= \"B\"").rules.front();
    second.geometry->driftChamber->wires = "material ~= \"metal\"";
    cfg.rules.push_back(second);
    REQUIRE_FALSE(geometry::optimizeDriftChambers(scene, cfg).hasErrors());
    CHECK_FALSE(warnings(geometry::optimizeDriftChambers(scene, cfg)));
}

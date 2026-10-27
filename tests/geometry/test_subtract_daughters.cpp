#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <config/predicate_parser.hpp>
#include <geometry/subtract_daughters.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <scene_build.hpp>
#include <tessellation/boolean_tessellator.hpp>
#include <tessellation/primitive_tessellator.hpp>
using namespace nodehammer;
using namespace nodehammer::ir::semantic;
namespace {
Scene fixture() {
    Scene s;
    s.materials.emplace(MaterialId{1}, SourceMaterial{MaterialId{1}, "carbon", std::nullopt, 0});
    s.materials.emplace(MaterialId{2}, SourceMaterial{MaterialId{2}, "foam", std::nullopt, 0});
    for (uint64_t i = 1; i <= 3; ++i) {
        s.shapes.emplace(ShapeId{i},
                         Shape{ShapeId{i}, BoxShape{double(4 - i), double(4 - i), double(4 - i)}});
        s.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "v" + std::to_string(i),
                                                     ShapeId{i}, MaterialId{i == 1 ? 1u : 2u}});
    }
    s.logVols.at(LogVolId{1}).daughters = {
        {"foam", LogVolId{2}, glm::translate(glm::dmat4{1}, glm::dvec3{1, 0, 0})}};
    s.logVols.at(LogVolId{2}).daughters = {{"core", LogVolId{3}}};
    s.root = {"vessel", LogVolId{1}};
    s.reseedIdCounters();
    return s;
}
config::NHConfig settings(std::string match = "name ~= \"vessel\"") {
    config::NHConfig cfg;
    config::Rule r;
    r.match = config::parsePredicateExpr(match).value();
    r.tessellation.emplace();
    r.tessellation->subtractDaughters = true;
    cfg.rules.push_back(r);
    return cfg;
}
const Shape &shape(const Scene &s, OccurrenceId route) {
    return s.shapes.at(s.logVols.at(s.makeTree()->materialize(route)->logVolId).shapeId);
}
double volume(const Scene &s, const Shape &shape) {
    tessellation::PrimitiveTessellator tess;
    auto mesh = tessellation::tessellateBooleanShape(shape.data, s, tess, {});
    REQUIRE_FALSE(mesh.diags.hasErrors());
    DiagnosticList diags;
    auto solid = tessellation::meshToManifold(mesh, diags, "test");
    REQUIRE(solid.has_value());
    return solid->Volume();
}
} // namespace
TEST_CASE("Daughter subtraction uses original full occupancy and local transforms",
          "[geometry][subtract_daughters]") {
    auto s = fixture();
    auto cfg = settings("true");
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(s.nodeCount() == 3);
    const auto &parent = std::get<BooleanSubtraction>(shape(s, {}).data);
    CHECK(parent.right == ShapeId{2}); // full daughter, not its carved shell
    CHECK(parent.rightTransform[3][0] == 1);
    CHECK(volume(s, shape(s, {})) == Catch::Approx(216 - 64));
    CHECK(volume(s, shape(s, {0})) == Catch::Approx(64 - 8));
    CHECK(s.makeTree()->materialize({0})->materialName == "foam");
    const auto bytes = sceneToBytes(s);
    auto reloaded = sceneFromBytes(bytes);
    REQUIRE_FALSE(geometry::subtractDaughters(reloaded, cfg).hasErrors());
    CHECK(sceneToBytes(reloaded) == bytes);
}
TEST_CASE("Daughter selectors and last matching disable isolate shared occurrences",
          "[geometry][subtract_daughters]") {
    auto s = fixture();
    s.shapes.emplace(ShapeId{4}, Shape{ShapeId{4}, BoxShape{20, 20, 20}});
    s.logVols.emplace(LogVolId{4}, LogicalVolume{LogVolId{4}, "world", ShapeId{4}, MaterialId{1}});
    s.logVols.at(LogVolId{4}).daughters = {
        {"A", LogVolId{1}}, {"B", LogVolId{1}}, {"C", LogVolId{1}}};
    s.root = {"world", LogVolId{4}};
    s.metadata.tagSets.push_back({{"label", "preserve"}});
    s.metadata.occurrenceTags[{0}] = 0;
    s.metadata.occurrenceTags[{2}] = 0;
    auto cfg = settings("name ~= \"A\" || name ~= \"B\" || name ~= \"C\"");
    cfg.rules[0].tessellation->subtractDaughtersMatch = "material ~= \"foam\"";
    auto disable = cfg.rules[0];
    disable.match = config::parsePredicateExpr("name ~= \"B\"").value();
    disable.tessellation->subtractDaughters = false;
    cfg.rules.push_back(disable);
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(std::holds_alternative<BooleanSubtraction>(shape(s, {0}).data));
    CHECK(std::holds_alternative<BoxShape>(shape(s, {1}).data));
    CHECK(s.makeTree()->materialize({0})->logVolId == s.makeTree()->materialize({2})->logVolId);
    CHECK(shape(s, {0, 0}).id == ShapeId{2});
    const auto bytes = sceneToBytes(s);
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(sceneToBytes(s) == bytes);
}
TEST_CASE("Display-excluded and skipped daughters still carve before the wedge",
          "[geometry][subtract_daughters][scene_build]") {
    const auto source = fixture();
    auto cfg = settings();
    config::Rule hide;
    hide.match = config::parsePredicateExpr("name ~= \"foam\"").value();
    hide.tessellation.emplace();
    hide.tessellation->skipGeometry = true;
    cfg.rules.push_back(hide);
    auto prep = pipeline::prepareSceneForTessellationFromInputs(cfg, source);
    CHECK(volume(prep.scene, shape(prep.scene, {})) == Catch::Approx(152));
    // Selection runs after material occupancy resolution, and cannot fill the hole.
    config::SelectionRule selection;
    selection.predicate = config::parsePredicateExpr("name ~= \"foam\"").value();
    selection.action = config::SelectionAction::DropIf;
    cfg.selection.push_back(selection);
    prep = pipeline::prepareSceneForTessellationFromInputs(cfg, source);
    CHECK(prep.scene.nodeCount() == 1);
    CHECK(volume(prep.scene, shape(prep.scene, {})) == Catch::Approx(152));
    prep = pipeline::prepareSceneForTessellationFromInputs(cfg, source,
                                                           tessellation::WedgeCutParams{0, 90});
    const auto carved = shape(prep.scene, {}).id;
    const auto cutVolume = prep.cutOverrides.volumes.at(ir::expanded::NodeId{1});
    const auto &cut = prep.scene.shapes.at(prep.scene.logVols.at(cutVolume).shapeId);
    CHECK(std::get<BooleanSubtraction>(cut.data).left == carved);
    // First-quadrant wedge removes54 from mother and24 from daughter.
    CHECK(volume(prep.scene, cut) == Catch::Approx(122));
}

TEST_CASE("IDEA vessel subtraction shares float boundaries with uncut foam",
          "[subtract_daughters][tessellation]") {
    Scene s;
    s.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, TubeShape{35.0000001, 199.9999999, 12.4999999}});
    s.shapes.emplace(ShapeId{2},
                     Shape{ShapeId{2}, TubeShape{35.0000002, 199.9999998, 11.749999906}});
    const BooleanSubtraction cut{ShapeId{1}, ShapeId{2}};
    tessellation::PrimitiveTessellator tess;
    auto mother = tessellation::tessellateBooleanShape(cut, s, tess, {});
    auto foam = tess.tessellate(s.shapes.at(ShapeId{2}).data, {});
    DiagnosticList ds;
    auto m = tessellation::meshToManifold(mother, ds, "mother");
    auto f = tessellation::meshToManifold(foam, ds, "foam");
    REQUIRE(m);
    REQUIRE(f);
    // Source clearances (1e-7cm) are below render-mesh float precision.
    // The two independently tessellated material regions must still be disjoint.
    // Before matching Boolean tube operands to primitive tessellation, overlap
    // measured 0.000623852cm^3 and caused visible depth fighting.
    CHECK(((*m) ^ (*f)).Volume() == Catch::Approx(0).margin(1e-9));
    for (const auto &v : mother.vertices) {
        CHECK(std::isfinite(v.normal.x));
        CHECK(std::isfinite(v.normal.y));
        CHECK(std::isfinite(v.normal.z));
    }
}

TEST_CASE("Volume-only daughter rules traverse shared definitions without expanding occurrences",
          "[subtract_daughters][geometry]") {
    auto s = fixture();
    // Twenty-six shared binary levels represent over 200 million placements
    // using fewer than thirty definitions, as in fibre calorimeter inputs.
    LogVolId child = s.root.logVolId;
    for (uint64_t i = 4; i < 30; ++i) {
        s.logVols.emplace(LogVolId{i},
                          LogicalVolume{LogVolId{i}, "shared", ShapeId{1}, MaterialId{1}});
        s.logVols.at(LogVolId{i}).daughters = {{"left", child}, {"right", child}};
        child = LogVolId{i};
    }
    s.root = {"world", child};
    s.reseedIdCounters();
    const auto count = s.nodeCount();
    auto cfg = settings("volume ~= \"v1\"");
    cfg.rules[0].tessellation->subtractDaughtersMatch = "material ~= \"foam\"";
    // An unrelated path-dependent tessellation rule must not disable the DAG path.
    config::Rule unrelated;
    unrelated.match = config::parsePredicateExpr("path ~= \"/world/left/**\"").value();
    unrelated.tessellation.emplace();
    unrelated.tessellation->maxSegmentsCircle = 12;
    cfg.rules.push_back(unrelated);
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(s.nodeCount() == count);
    CHECK(s.logVols.size() == 29);
    OccurrenceId route(26, 0);
    CHECK(std::holds_alternative<BooleanSubtraction>(shape(s, route).data));
    const auto root = s.logVols.at(s.root.logVolId);
    CHECK(root.daughters[0].logVolId == root.daughters[1].logVolId);
}
TEST_CASE("Definition memoization preserves sparse occurrence metadata on every reused branch",
          "[subtract_daughters][geometry]") {
    auto s = fixture();
    s.logVols.emplace(LogVolId{4}, LogicalVolume{LogVolId{4}, "world", ShapeId{1}, MaterialId{1}});
    s.logVols.at(LogVolId{4}).daughters = {{"A", LogVolId{1}}, {"B", LogVolId{1}}};
    s.root = {"world", LogVolId{4}};
    s.metadata.tagSets.push_back({{"custom", "left"}});
    s.metadata.tagSets.push_back({{"custom", "right"}});
    s.metadata.occurrenceTags[{0}] = 0;
    s.metadata.occurrenceTags[{1}] = 1;
    auto cfg = settings("volume ~= \"v1\"");
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(s.makeTree()->materialize({0})->tags.at("custom") == "left");
    CHECK(s.makeTree()->materialize({1})->tags.at("custom") == "right");
    const auto bytes = sceneToBytes(s);
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(sceneToBytes(s) == bytes);
}

TEST_CASE("Path-dependent subtraction overrides stay occurrence-local",
          "[subtract_daughters][geometry]") {
    auto s = fixture();
    s.logVols.emplace(LogVolId{4}, LogicalVolume{LogVolId{4}, "world", ShapeId{1}, MaterialId{1}});
    s.logVols.at(LogVolId{4}).daughters = {{"A", LogVolId{1}}, {"B", LogVolId{1}}};
    s.root = {"world", LogVolId{4}};
    auto cfg = settings("volume ~= \"v1\"");
    config::Rule disable;
    disable.match = config::parsePredicateExpr("path ~= \"/world/B\"").value();
    disable.tessellation.emplace();
    disable.tessellation->subtractDaughters = false;
    cfg.rules.push_back(disable);
    REQUIRE_FALSE(geometry::subtractDaughters(s, cfg).hasErrors());
    CHECK(std::holds_alternative<BooleanSubtraction>(shape(s, {0}).data));
    CHECK(std::holds_alternative<BoxShape>(shape(s, {1}).data));
}

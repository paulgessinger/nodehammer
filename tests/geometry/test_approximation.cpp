#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <config/predicate_parser.hpp>
#include <geometry/approximation.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <ir/semantic/occurrence_tree.hpp>

using namespace nodehammer;
using namespace nodehammer::ir::semantic;
using namespace nodehammer::config;

namespace {
Scene cells() {
    Scene s;
    const MaterialId material{1};
    s.materials.emplace(material, SourceMaterial{material, "glass", std::nullopt, 2.5});
    s.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{100, 100, 100}});
    s.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{1, 2, 3}});
    s.logVols.emplace(LogVolId{1}, LogicalVolume{LogVolId{1}, "world", ShapeId{1}, material});
    s.logVols.emplace(LogVolId{2}, LogicalVolume{LogVolId{2}, "module", ShapeId{1}, material});
    s.logVols.emplace(LogVolId{3}, LogicalVolume{LogVolId{3}, "cell", ShapeId{2}, material});
    auto translate = [](double x) { return glm::translate(glm::dmat4{1}, glm::dvec3{x, 0, 0}); };
    s.logVols.at(LogVolId{1}).daughters = {{"A", LogVolId{2}, translate(20)},
                                           {"B", LogVolId{2}, translate(-20)}};
    s.logVols.at(LogVolId{2}).daughters = {{"cell0", LogVolId{3}, translate(-2)},
                                           {"cell1", LogVolId{3}, translate(2)}};
    s.root = {"world", LogVolId{1}, glm::dmat4{1}};
    s.reseedIdCounters();
    return s;
}
Rule rule(std::string_view predicate) {
    auto match = parsePredicateExpr(predicate);
    REQUIRE(match.has_value());
    Rule r;
    r.match = *match;
    return r;
}
NHConfig hullConfig(std::string_view predicate = "name ~= \"A\"") {
    NHConfig cfg;
    auto skip = rule("name ~= \"A\" || name ~= \"B\" || name ~= \"world\"");
    skip.tessellation.emplace();
    skip.tessellation->skipGeometry = true;
    cfg.rules.push_back(skip);
    auto hull = rule(predicate);
    hull.geometry = Rule::Geometry{"convex_hull", std::nullopt};
    cfg.rules.push_back(hull);
    return cfg;
}
const TessellatedShape &hull(const Scene &s, const OccurrenceId &id) {
    auto occurrence = s.makeTree()->materialize(id);
    const auto &shape = s.shapes.at(s.logVols.at(occurrence->logVolId).shapeId);
    REQUIRE(std::holds_alternative<TessellatedShape>(shape.data));
    return std::get<TessellatedShape>(shape.data);
}
void bounds(const TessellatedShape &shape, glm::dvec3 expectedMin, glm::dvec3 expectedMax) {
    REQUIRE_FALSE(shape.triangles.empty());
    auto min = shape.triangles.front().vertices.front();
    auto max = min;
    for (const auto &triangle : shape.triangles)
        for (const auto &v : triangle.vertices) {
            min = glm::min(min, v);
            max = glm::max(max, v);
        }
    for (int i = 0; i < 3; ++i) {
        CHECK(min[i] == Catch::Approx(expectedMin[i]));
        CHECK(max[i] == Catch::Approx(expectedMax[i]));
    }
}
} // namespace

TEST_CASE("Geometry approximation isolates shared occurrences and preserves metadata",
          "[geometry][approximation]") {
    auto s = cells();
    s.metadata.originalPaths.emplace(OccurrenceId{0}, "/source/moduleA");
    s.metadata.tagSets = {{{"subsystem", "ecal"}}};
    s.metadata.occurrenceTags.emplace(OccurrenceId{0}, 0);
    auto oldA = s.makeTree()->materialize({0});
    auto oldB = s.makeTree()->materialize({1});
    const auto diags = geometry::approximate(s, hullConfig());
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(s.validate() == 5);
    auto a = s.makeTree()->materialize({0});
    auto b = s.makeTree()->materialize({1});
    CHECK(a->name == oldA->name);
    CHECK(a->localTransform == oldA->localTransform);
    CHECK(a->worldTransform == oldA->worldTransform);
    CHECK(a->originalPath == oldA->originalPath);
    auto expectedTags = oldA->tags;
    expectedTags["nodehammer.geometry"] = "convex_hull";
    CHECK(a->tags == expectedTags);
    CHECK(a->childCount == 0);
    CHECK(b->childCount == 2);
    CHECK(b->localTransform == oldB->localTransform);
    CHECK(b->logVolId == oldB->logVolId);
    // Parent's 100-unit envelope must not enter the fitted hull.
    bounds(hull(s, {0}), {-3, -2, -3}, {3, 2, 3});
    const auto bytes = sceneToBytes(s);
    auto restored = sceneFromBytes(bytes);
    CHECK(restored.validate() == 5);
    CHECK(sceneToBytes(restored) == bytes);
    CHECK(restored.makeTree()->materialize({0})->tags == expectedTags);
    REQUIRE_FALSE(geometry::approximate(restored, hullConfig()).hasErrors());
    CHECK(sceneToBytes(restored) == bytes);
}

TEST_CASE("Geometry approximation uses outermost matching subtree", "[geometry][approximation]") {
    auto s = cells();
    REQUIRE_FALSE(geometry::approximate(s, hullConfig("true")).hasErrors());
    REQUIRE(s.validate() == 1);
    CHECK(s.root.name == "world");
    bounds(hull(s, {}), {-23, -2, -3}, {23, 2, 3});
}

TEST_CASE("Geometry approximation allows leaf replacements", "[geometry][approximation]") {
    auto s = cells();
    REQUIRE_FALSE(geometry::approximate(s, hullConfig("name ~= \"cell0\"")).hasErrors());
    CHECK(s.validate() == 7);
    bounds(hull(s, {0, 0}), {-1, -2, -3}, {1, 2, 3});
}

TEST_CASE("Geometry approximation rejects empty or coplanar hull without mutation",
          "[geometry][approximation]") {
    auto s = cells();
    auto cfg = hullConfig();
    SECTION("empty") {
        auto skip = rule("true");
        skip.tessellation.emplace();
        skip.tessellation->skipGeometry = true;
        cfg.rules.push_back(skip);
    }
    SECTION("coplanar") { s.shapes.at(ShapeId{2}).data = BoxShape{1, 2, 0}; }
    const auto before = sceneToBytes(s);
    CHECK(geometry::approximate(s, cfg).hasErrors());
    CHECK(sceneToBytes(s) == before);
}

TEST_CASE("Geometry approximation reuses equivalent hulls but respects occurrence-specific skips",
          "[geometry][approximation]") {
    auto s = cells();
    auto cfg = hullConfig("name ~= \"A\" || name ~= \"B\"");
    bool different = false;
    SECTION("equivalent modules") {}
    SECTION("one module excludes one cell") {
        different = true;
        auto skip = rule("path ~= \"/world/B/cell1\"");
        skip.tessellation.emplace();
        skip.tessellation->skipGeometry = true;
        cfg.rules.push_back(skip);
    }
    REQUIRE_FALSE(geometry::approximate(s, cfg).hasErrors());
    REQUIRE(s.validate() == 3);
    const auto a = s.makeTree()->materialize({0});
    const auto b = s.makeTree()->materialize({1});
    const auto aShape = s.logVols.at(a->logVolId).shapeId;
    const auto bShape = s.logVols.at(b->logVolId).shapeId;
    if (different) {
        CHECK(aShape != bShape);
        bounds(hull(s, {1}), {-3, -2, -3}, {-1, 2, 3});
    } else {
        CHECK(aShape == bShape);
        bounds(hull(s, {1}), {-3, -2, -3}, {3, 2, 3});
    }
}

TEST_CASE("Geometry approximation transforms corners in module-local coordinates",
          "[geometry][approximation]") {
    auto s = cells();
    // A quarter-turn around z followed by nonuniform scaling in the cell frame.
    // Local half-extents (1,2,3) become (6,2,12), then translated to x=5.
    auto rotation = glm::dmat4{1};
    rotation[0] = {0, 1, 0, 0};
    rotation[1] = {-1, 0, 0, 0};
    const auto transform = glm::translate(glm::dmat4{1}, glm::dvec3{5, 0, 0}) * rotation *
                           glm::scale(glm::dmat4{1}, glm::dvec3{2, 3, 4});
    s.logVols.at(LogVolId{2}).daughters = {{"cell", LogVolId{3}, transform}};
    const auto placement = s.makeTree()->materialize({0})->worldTransform;
    REQUIRE_FALSE(geometry::approximate(s, hullConfig()).hasErrors());
    bounds(hull(s, {0}), {-1, -2, -12}, {11, 2, 12});
    CHECK(s.makeTree()->materialize({0})->worldTransform == placement);
}

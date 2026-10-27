#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <config/predicate_parser.hpp>
#include <geometry/drift_chamber.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <ir/expanded/adapt.hpp>
#include <ir/expanded/conversion.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>
#include <tessellation/wire_cut.hpp>

using namespace nodehammer;
using namespace nodehammer::ir::semantic;
using namespace nodehammer::tessellation;
namespace {
glm::dmat4 alongY(double x = 10) {
    return glm::translate(glm::dmat4{1}, glm::dvec3{x, 0, 0}) *
           glm::rotate(glm::dmat4{1}, -std::numbers::pi / 2, glm::dvec3{1, 0, 0});
}
TubeShape sector(double start, double width) {
    return {0, 100, 100, start * std::numbers::pi / 180, width * std::numbers::pi / 180};
}
Scene wires() {
    Scene scene;
    scene.materials.emplace(MaterialId{1}, SourceMaterial{MaterialId{1}, "metal", std::nullopt, 1});
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, TessellatedShape{}});
    scene.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, TubeShape{0, 0.01, 20}});
    scene.logVols.emplace(LogVolId{1},
                          LogicalVolume{LogVolId{1}, "world", ShapeId{1}, MaterialId{1}});
    scene.logVols.emplace(LogVolId{2},
                          LogicalVolume{LogVolId{2}, "wire", ShapeId{2}, MaterialId{1}});
    scene.logVols.at(LogVolId{1}).daughters = {{"w1", LogVolId{2}, alongY(10)},
                                               {"w2", LogVolId{2}, alongY(15)}};
    scene.root = {"world", LogVolId{1}};
    scene.metadata.tagSets = {{{geometry::kDriftRoleTag, "wire"}}};
    scene.metadata.volumeTags[LogVolId{2}] = 0;
    scene.reseedIdCounters();
    return scene;
}
config::NHConfig fastConfig() {
    config::NHConfig cfg;
    cfg.tessellationDefaults.maxSegmentsCircle = 12;
    config::Rule root;
    root.match = config::parsePredicateExpr("name ~= \"world\"").value();
    root.tessellation.emplace();
    root.tessellation->skipGeometry = true;
    cfg.rules.push_back(root);
    config::Rule r;
    r.match = config::parsePredicateExpr("tag.nodehammer_drift_role == \"wire\"").value();
    r.tessellation.emplace();
    r.tessellation->wireCut = "centerline";
    r.tessellation->wireLodPixels = 0.7;
    r.extras = detail::JsonValue::makeObject({{"user_note", "preserved"}});
    cfg.rules.push_back(r);
    return cfg;
}
} // namespace
TEST_CASE("Wire centerline clips wraparound and large azimuthal sectors", "[wire_cut]") {
    const TubeShape wire{0, 0.01, 20};
    SECTION("quarter") {
        auto kept = clipWireCenterline(wire, sector(0, 90), alongY());
        REQUIRE(kept.has_value());
        REQUIRE(kept->size() == 1);
        CHECK(kept->front().lo == Catch::Approx(-20));
        CHECK(kept->front().hi == Catch::Approx(0).margin(1e-12));
    }
    SECTION("wrap gives two disconnected spans") {
        auto kept = clipWireCenterline(wire, sector(315, 90), alongY());
        REQUIRE(kept.has_value());
        REQUIRE(kept->size() == 2);
        CHECK((*kept)[0].lo == Catch::Approx(-20));
        CHECK((*kept)[0].hi == Catch::Approx(-10));
        CHECK((*kept)[1].lo == Catch::Approx(10));
        CHECK((*kept)[1].hi == Catch::Approx(20));
    }
    SECTION("large removed sector leaves middle span") {
        auto kept = clipWireCenterline(wire, sector(45, 270), alongY());
        REQUIRE(kept.has_value());
        REQUIRE(kept->size() == 1);
        CHECK(kept->front().lo == Catch::Approx(-10));
        CHECK(kept->front().hi == Catch::Approx(10));
    }
}
TEST_CASE("Wire approximation falls back for grazing and unsupported geometry", "[wire_cut]") {
    const TubeShape wire{0, 0.01, 20};
    CHECK_FALSE(clipWireCenterline(wire, sector(0, 90), alongY(0.001)));
    CHECK_FALSE(clipWireCenterline(wire, sector(0, 90),
                                   glm::translate(glm::dmat4{1}, glm::dvec3{10, 0, 0})));
    CHECK_FALSE(clipWireCenterline(wire, sector(0, 90), glm::scale(alongY(), glm::dvec3{2})));
    auto hollow = wire;
    hollow.rMin = 0.001;
    CHECK_FALSE(clipWireCenterline(hollow, sector(0, 90), alongY()));
    auto bounded = sector(0, 90);
    bounded.rMax = 10;
    CHECK_FALSE(clipWireCenterline(wire, bounded, alongY()));
}
TEST_CASE("Opted-in cut wires share one mesh and preserve material and extras",
          "[wire_cut][tessellation]") {
    auto scene = wires();
    auto cfg = fastConfig();
    OccurrenceOverrides overrides;
    const auto stats = applyWedgeCut(scene, {0, 90}, overrides);
    REQUIRE(stats.cut == 2);
    auto lowered = TessellationPass(cfg).lower(scene, &overrides);
    REQUIRE_FALSE(lowered.diags.hasErrors());
    REQUIRE(lowered.scene.meshAssets.size() == 1);
    CHECK(lowered.scene.meshAssets.begin()->second.provenance.sourceSystem ==
          "tessellation_pass/wire_centerline");
    std::size_t drawn = 0;
    for (const auto &[id, node] : lowered.scene.nodes) {
        (void)id;
        if (node.meshBindings.empty())
            continue;
        ++drawn;
        CHECK(node.extras.at("user_note") == detail::JsonValue{"preserved"});
        CHECK(node.extras.at("nodehammer_wire_lod").at("radius") == detail::JsonValue{1.0});
        CHECK(node.extras.at("nodehammer_wire_lod").at("half_length") == detail::JsonValue{1.0});
        CHECK(node.worldTransform[3].y == Catch::Approx(-10));
        CHECK(glm::length(glm::vec3(node.worldTransform[2])) == Catch::Approx(10));
    }
    CHECK(drawn == 2);
    SECTION("exact is default") {
        cfg.rules.back().tessellation->wireCut.reset();
        auto exact = TessellationPass(cfg).lower(scene, &overrides);
        REQUIRE_FALSE(exact.diags.hasErrors());
        for (const auto &[id, mesh] : exact.scene.meshAssets) {
            (void)id;
            CHECK(mesh.provenance.sourceSystem != "tessellation_pass/wire_centerline");
        }
    }
    SECTION("no generated provenance means arbitrary source Boolean stays exact") {
        overrides.wedgeShapeId.reset();
        auto exact = TessellationPass(cfg).lower(scene, &overrides);
        REQUIRE_FALSE(exact.diags.hasErrors());
        for (const auto &[id, mesh] : exact.scene.meshAssets) {
            (void)id;
            CHECK(mesh.provenance.sourceSystem != "tessellation_pass/wire_centerline");
        }
    }
}
TEST_CASE("Two wire spans retain parent hierarchy with shared cylinder",
          "[wire_cut][tessellation]") {
    auto scene = wires();
    OccurrenceOverrides overrides;
    (void)applyWedgeCut(scene, {315, 45}, overrides);
    auto lowered = TessellationPass(fastConfig()).lower(scene, &overrides);
    REQUIRE_FALSE(lowered.diags.hasErrors());
    REQUIRE(lowered.scene.meshAssets.size() == 1);
    std::size_t spans = 0;
    for (const auto &[id, node] : lowered.scene.nodes) {
        if (node.meshBindings.empty())
            continue;
        ++spans;
        REQUIRE(node.parentId);
        const auto &parent = lowered.scene.nodes.at(*node.parentId);
        CHECK(parent.children.size() == 2);
        const auto expected = parent.worldTransform * node.localTransform;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                CHECK(expected[column][row] ==
                      Catch::Approx(node.worldTransform[column][row]).margin(1e-6));
    }
    CHECK(spans == 4);
}
TEST_CASE("Expanded wedge provenance persists through semantic NHB conversion", "[wire_cut]") {
    auto expanded = expand(wires());
    (void)applyWedgeCut(expanded, {0, 90});
    const auto cfg = fastConfig();
    auto lowered = TessellationPass(cfg).lower(expanded);
    REQUIRE_FALSE(lowered.diags.hasErrors());
    REQUIRE(lowered.scene.meshAssets.size() == 1);
    auto restored = sceneFromBytes(sceneToBytes(fromExpanded(expanded)));
    auto reloaded = TessellationPass(cfg).lower(restored);
    REQUIRE_FALSE(reloaded.diags.hasErrors());
    REQUIRE(reloaded.scene.meshAssets.size() == 1);
    CHECK(reloaded.scene.meshAssets.begin()->second.provenance.sourceSystem ==
          "tessellation_pass/wire_centerline");
}
TEST_CASE("Uncut opted-in wires use shared canonical cylinder and exact LOD can be disabled",
          "[wire_cut]") {
    const auto scene = wires();
    auto cfg = fastConfig();
    auto lowered = TessellationPass(cfg).lower(scene);
    REQUIRE(lowered.scene.meshAssets.size() == 1);
    CHECK(lowered.scene.meshAssets.begin()->second.provenance.sourceSystem ==
          "tessellation_pass/wire_centerline");
    cfg.rules.back().tessellation->wireCut = "exact";
    cfg.rules.back().tessellation->wireLodPixels = 0;
    auto exact = TessellationPass(cfg).lower(scene);
    REQUIRE_FALSE(exact.diags.hasErrors());
    for (const auto &[id, node] : exact.scene.nodes) {
        (void)id;
        CHECK(node.extras.find("nodehammer_wire_lod") == nullptr);
    }
}

TEST_CASE("Wire policy resolves defaults and last matching overrides into render metadata",
          "[wire_cut]") {
    auto cfg = fastConfig();
    cfg.tessellationDefaults.wireLodDistance = 40;
    cfg.tessellationDefaults.wireLodFractions = {1, 0.4, 0.2, 0.01};
    cfg.tessellationDefaults.wireLodDistanceFactors = {0.1, 0.2, 1};
    cfg.tessellationDefaults.wireLodHintPixels = 0.8;
    auto &rule = cfg.rules.back().tessellation;
    rule->wireLodFractions = {0.8, 0.3, 0.1, 0};
    rule->wireLodHintPixels = 0;
    const auto lowered = TessellationPass(cfg).lower(wires());
    REQUIRE_FALSE(lowered.diags.hasErrors());
    int seen = 0;
    for (const auto &[id, node] : lowered.scene.nodes) {
        const auto *metadata = node.extras.find("nodehammer_wire_lod");
        if (!metadata)
            continue;
        ++seen;
        REQUIRE(metadata->find("fractions"));
        const auto *fractions = metadata->find("fractions")->asArray();
        REQUIRE(fractions);
        REQUIRE(fractions->size() == 4);
        CHECK(*(*fractions)[0].asDouble() == 0.8);
        CHECK(*(*fractions)[3].asDouble() == 0);
        CHECK(*metadata->find("hint_pixels")->asDouble() == 0);
        CHECK(*metadata->find("max_distance")->asDouble() == 40);
        CHECK(*(*metadata->find("distance_factors")->asArray())[1].asDouble() == 0.2);
    }
    CHECK(seen > 0);
}

TEST_CASE("Wire cylinder cache respects shading policy", "[wire_cut][shading]") {
    auto cfg = fastConfig();
    config::Rule flat;
    flat.match = config::parsePredicateExpr("name == \"w2\"").value();
    flat.tessellation.emplace();
    flat.tessellation->shading = config::Shading::Flat;
    cfg.rules.push_back(flat);
    auto result = TessellationPass(cfg).lower(wires());
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.scene.meshAssets.size() == 2);
    for (const auto &[id, node] : result.scene.nodes) {
        if (node.name != "w2")
            continue;
        REQUIRE(node.meshBindings.size() == 1);
        const auto &mesh = result.scene.meshAssets.at(node.meshBindings.front().meshId);
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto &a = mesh.vertices[mesh.indices[i]];
            const auto &b = mesh.vertices[mesh.indices[i + 1]];
            const auto &c = mesh.vertices[mesh.indices[i + 2]];
            CHECK(a.normal == b.normal);
            CHECK(a.normal == c.normal);
        }
    }
}

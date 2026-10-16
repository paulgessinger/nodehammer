#include <catch2/catch_test_macros.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <config/config_writer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <ir/semantic_json.hpp>
#include <limits>
#include <tessellation/generic_trap.hpp>
#include <tessellation/primitive_tessellator.hpp>
#include <tessellation/tessellation_pass.hpp>
using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::tessellation;
namespace {
semantic::GenericTrapShape trap() {
    return {3, {{{-2, -1}, {2, -1}, {2, 1}, {-2, 1}, {-1, -2}, {2, -1}, {1, 2}, {-2, 1}}}};
}
semantic::Scene scene(bool boolean = false, bool merged = false) {
    semantic::Scene s;
    s.shapes.emplace(semantic::ShapeId{1}, semantic::Shape{{1}, trap()});
    s.shapes.emplace(semantic::ShapeId{2}, semantic::Shape{{2}, semantic::BoxShape{0.3, 0.3, 5}});
    s.shapes.emplace(semantic::ShapeId{3},
                     semantic::Shape{{3}, semantic::BooleanSubtraction{{1}, {2}}});
    s.materials.emplace(semantic::MaterialId{1},
                        semantic::SourceMaterial{{1}, "test", glm::vec3{1}, 1});
    const semantic::ShapeId id{boolean ? 3u : 1u};
    s.logVols.emplace(semantic::LogVolId{1}, semantic::LogicalVolume{{1}, "trap", id, {1}});
    semantic::LogicalVolume root{{2}, "root", {2}, {1}};
    root.daughters = {{"coarse", {1}, glm::dmat4{1}}, {"fine", {1}, glm::dmat4{1}}};
    if (merged) {
        semantic::LogicalVolume group{{3}, "group", {2}, {1}};
        group.daughters = {{"trap", {1}, glm::dmat4{1}}};
        s.logVols.emplace(semantic::LogVolId{3}, group);
        root.daughters[0].logVolId = {3};
        root.daughters[1].logVolId = {3};
    }
    s.logVols.emplace(semantic::LogVolId{2}, root);
    s.root = {"root", {2}, glm::dmat4{1}};
    return s;
}
} // namespace
TEST_CASE("GenericTrap parameters roundtrip without meshes", "[generic_trap][ir]") {
    auto s = scene();
    auto bytes = semantic::sceneToBytes(s);
    REQUIRE(bytes.size() < 10000);
    auto loaded = FlatBufferImporter::importFromBytes("trap.nhb", bytes);
    REQUIRE(semantic::sceneToBytes(loaded.scene) == bytes);
    const auto &actual = std::get<semantic::GenericTrapShape>(loaded.scene.shapes.at({1}).data);
    REQUIRE(actual.dz == trap().dz);
    REQUIRE(actual.vertices == trap().vertices);
    nlohmann::json json = s.shapes.at({1});
    auto restored = json.get<semantic::Shape>();
    REQUIRE(std::get<semantic::GenericTrapShape>(restored.data).vertices == trap().vertices);
    auto coarse = PrimitiveTessellator{}.tessellate(actual, {.genericTrapTolerance = 0.1});
    auto fine = PrimitiveTessellator{}.tessellate(actual, {.genericTrapTolerance = 0.001});
    REQUIRE(coarse.succeeded);
    REQUIRE(fine.succeeded);
    REQUIRE(fine.indices.size() > coarse.indices.size());
    REQUIRE(semantic::sceneToBytes(loaded.scene) == bytes);
    auto bad = PrimitiveTessellator{}.tessellate(actual, {.genericTrapTolerance = 0});
    REQUIRE_FALSE(bad.succeeded);
    REQUIRE(bad.diags.hasErrors());
}
TEST_CASE("GenericTrap tolerance participates in primitive boolean and merge caches",
          "[generic_trap][pass]") {
    bool boolean = false, merged = false;
    SECTION("primitive") {}
    SECTION("boolean operand") { boolean = true; }
    SECTION("merged descendants") { merged = true; }
    auto s = scene(boolean, merged);
    const auto text = std::string{R"(
[defaults.tessellation]
generic_trap_tolerance=0.1
[[rules]]
match='name == "root"'
[rules.tessellation]
skip_geometry=true
[[rules]]
match='path ~= "**/fine/**" || name == "fine"'
[rules.tessellation]
generic_trap_tolerance=0.001
)"} + (merged ? R"(
[[rules]]
match='name == "coarse" || name == "fine"'
[rules.tessellation]
merge_descendants=true
)"
              : "");
    auto cfg = config::ConfigLoader::loadFromString(text).config;
    auto roundtrip = config::ConfigLoader::loadFromString(config::configToToml(cfg)).config;
    REQUIRE(roundtrip.tessellationDefaults.genericTrapTolerance == 0.1);
    REQUIRE(roundtrip.rules[1].tessellation->genericTrapTolerance == 0.001);
    auto result = TessellationPass{roundtrip}.lower(s);
    REQUIRE_FALSE(result.diags.hasErrors());
    size_t coarseCount = 0, fineCount = 0;
    for (const auto &[id, node] : result.scene.nodes) {
        if (node.name != "coarse" && node.name != "fine")
            continue;
        REQUIRE(node.meshBindings.size() == 1);
        const auto count = result.scene.meshAssets.at(node.meshBindings[0].meshId).indices.size();
        if (node.name == "coarse")
            coarseCount = count;
        else
            fineCount = count;
    }
    REQUIRE(coarseCount > 0);
    REQUIRE(fineCount > coarseCount);
    cfg.tessellationDefaults.genericTrapTolerance = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(config::ConfigValidator::validate(cfg).hasErrors());
}

TEST_CASE("GenericTrap dedup compares all parameters", "[generic_trap][ir]") {
    auto s = scene();
    s.shapes.emplace(semantic::ShapeId{4}, semantic::Shape{{4}, trap()});
    auto different = trap();
    different.vertices[6].x += 0.5;
    s.shapes.emplace(semantic::ShapeId{5}, semantic::Shape{{5}, different});
    REQUIRE(s.deduplicateShapes() == 1);
    REQUIRE(s.shapes.size() == 4);
}

#include <catch2/catch_test_macros.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <config/config_writer.hpp>
#include <tessellation/primitive_tessellator.hpp>
#include <tessellation/shading.hpp>
#include <tessellation/tessellation_pass.hpp>

#include <cmath>
#include <limits>

using namespace nodehammer;
using namespace nodehammer::tessellation;

namespace {
TessellationOutput bentFaces() {
    TessellationOutput mesh;
    // Two faces at 20 degrees, with an unwelded common edge.
    const float angle = glm::radians(20.f);
    for (const auto p :
         {glm::vec3{0, 0, 0}, glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}, glm::vec3{1, 0, 0},
          glm::vec3{0, 0, 0}, glm::vec3{0, -std::cos(angle), std::sin(angle)}}) {
        mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
        mesh.vertices.push_back({p, {1, 0, 0}});
    }
    return mesh;
}

bool hasSmoothCorners(const ir::render::MeshAsset &mesh) {
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto &a = mesh.vertices[mesh.indices[i]];
        const auto &b = mesh.vertices[mesh.indices[i + 1]];
        const auto &c = mesh.vertices[mesh.indices[i + 2]];
        const auto cross = glm::cross(b.position - a.position, c.position - a.position);
        if (glm::length(cross) > 0 && glm::dot(glm::normalize(cross), a.normal) < 0.999f)
            return true;
    }
    return false;
}

ir::semantic::Scene sharedScene(bool boolean, bool merged) {
    using namespace ir::semantic;
    Scene s;
    s.shapes.emplace(ShapeId{1}, Shape{{1}, TubeShape{0, 5, 10}});
    s.shapes.emplace(ShapeId{2}, Shape{{2}, BoxShape{1, 8, 12}});
    s.shapes.emplace(ShapeId{3}, Shape{{3}, BooleanSubtraction{{1}, {2}}});
    s.materials.emplace(MaterialId{1}, SourceMaterial{{1}, "test", glm::vec3{1}, 1});
    s.logVols.emplace(LogVolId{1}, LogicalVolume{{1}, "tube", {boolean ? 3u : 1u}, {1}});
    LogicalVolume root{{2}, "root", {2}, {1}};
    root.daughters = {{"sharp", {1}, glm::dmat4{1}}, {"smooth", {1}, glm::dmat4{1}}};
    if (merged) {
        LogicalVolume group{{3}, "group", {2}, {1}};
        group.daughters = {{"tube", {1}, glm::dmat4{1}}};
        s.logVols.emplace(LogVolId{3}, group);
        root.daughters[0].logVolId = {3};
        root.daughters[1].logVolId = {3};
    }
    s.logVols.emplace(LogVolId{2}, root);
    s.root = {"root", {2}, glm::dmat4{1}};
    return s;
}
} // namespace

TEST_CASE("Shading preserves geometry and respects the crease threshold", "[shading]") {
    auto original = bentFaces();
    auto mesh = original;
    applyShading(mesh, config::Shading::Preserve, 30);
    REQUIRE(mesh.indices == original.indices);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        REQUIRE(mesh.vertices[i].position == original.vertices[i].position);
        REQUIRE(mesh.vertices[i].normal == original.vertices[i].normal);
    }
    SECTION("smooth joins an unwelded edge") {
        applyShading(mesh, config::Shading::Smooth, 30);
        REQUIRE(mesh.vertices.size() == 4);
        REQUIRE(mesh.indices[0] == mesh.indices[4]);
        REQUIRE(mesh.indices[1] == mesh.indices[3]);
        REQUIRE(mesh.vertices[mesh.indices[0]].normal.y > 0.01f);
        REQUIRE(mesh.vertices[mesh.indices[0]].normal.z > 0.9f);
    }
    SECTION("angle below crease keeps it sharp") {
        applyShading(mesh, config::Shading::Smooth, 10);
        REQUIRE(mesh.vertices.size() == 6);
        REQUIRE(mesh.vertices[mesh.indices[0]].normal == glm::vec3{0, 0, 1});
    }
    SECTION("flat splits all corners") {
        applyShading(mesh, config::Shading::Flat, 180);
        REQUIRE(mesh.vertices.size() == 6);
        REQUIRE(mesh.vertices[mesh.indices[0]].normal == glm::vec3{0, 0, 1});
    }
    REQUIRE(mesh.indices.size() == original.indices.size());
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        REQUIRE(mesh.vertices[mesh.indices[i]].position ==
                original.vertices[original.indices[i]].position);
        REQUIRE(std::abs(glm::length(mesh.vertices[mesh.indices[i]].normal) - 1.f) < 1e-6f);
    }
}

TEST_CASE("Smooth shading preserves box corners and does not join vertex-only contacts",
          "[shading]") {
    auto box = PrimitiveTessellator{}.tessellate(ir::semantic::BoxShape{1, 2, 3}, {});
    applyShading(box, config::Shading::Smooth, 30);
    REQUIRE(box.vertices.size() == 24);
    for (const auto &v : box.vertices)
        REQUIRE(std::abs(v.normal.x) + std::abs(v.normal.y) + std::abs(v.normal.z) == 1.f);

    auto mesh = bentFaces();
    mesh.vertices[3].position = {2, 0, 0}; // only the origin remains shared
    applyShading(mesh, config::Shading::Smooth, 180);
    REQUIRE(mesh.vertices.size() == 6);
}

TEST_CASE("Degenerate triangles remain finite", "[shading]") {
    auto mesh = bentFaces();
    mesh.vertices[1].position = mesh.vertices[0].position;
    applyShading(mesh, config::Shading::Smooth, 30);
    for (const auto &v : mesh.vertices) {
        REQUIRE(std::isfinite(v.normal.x));
        REQUIRE(std::isfinite(v.normal.y));
        REQUIRE(std::isfinite(v.normal.z));
    }
}

TEST_CASE("Shading settings resolve per field and distinguish shared shape and merge caches",
          "[shading][pass]") {
    bool boolean = false, merged = false;
    SECTION("primitive") {}
    SECTION("boolean") { boolean = true; }
    SECTION("merged") { merged = true; }
    const auto source = sharedScene(boolean, merged);
    for (const bool overrideMode : {false, true}) {
        auto text = std::string{R"(
[defaults.tessellation]
max_segments_circle = 32
shading = "smooth"
smooth_angle = 5
[[rules]]
match = 'name == "root"'
[rules.tessellation]
skip_geometry = true
[[rules]]
match = 'name == "smooth" || path ~= "**/smooth/**"'
[rules.tessellation]
smooth_angle = 30
)"};
        if (merged)
            text += R"(
[[rules]]
match = 'name == "sharp" || name == "smooth"'
[rules.tessellation]
merge_descendants = true
)";
        auto cfg = config::ConfigLoader::loadFromString(text).config;
        if (overrideMode) {
            cfg.tessellationDefaults.shading = config::Shading::Flat;
            cfg.rules[1].tessellation->shading = config::Shading::Smooth;
        }
        const auto result = TessellationPass{cfg}.lower(source);
        REQUIRE_FALSE(result.diags.hasErrors());
        ir::render::MeshAssetId sharpId{}, smoothId{};
        for (const auto &[id, node] : result.scene.nodes) {
            if (node.name != "sharp" && node.name != "smooth")
                continue;
            REQUIRE(node.meshBindings.size() == 1);
            const auto meshId = node.meshBindings[0].meshId;
            const auto &mesh = result.scene.meshAssets.at(meshId);
            REQUIRE_FALSE(mesh.indices.empty());
            REQUIRE(hasSmoothCorners(mesh) == (node.name == "smooth"));
            (node.name == "smooth" ? smoothId : sharpId) = meshId;
        }
        REQUIRE(sharpId.value != 0);
        REQUIRE(smoothId.value != 0);
        REQUIRE(sharpId != smoothId);
    }
}

TEST_CASE("TOML shading configs roundtrip and reject invalid settings", "[shading][config]") {
    const auto toml = config::ConfigLoader::loadFromString(R"(
[defaults.tessellation]
shading = "smooth"
smooth_angle = 45
[[rules]]
[rules.tessellation]
shading = "preserve"
smooth_angle = 0
)");
    REQUIRE_FALSE(toml.diags.hasErrors());
    const auto restored = config::ConfigLoader::loadFromString(config::configToToml(toml.config));
    REQUIRE(restored.config.tessellationDefaults.shading == config::Shading::Smooth);
    REQUIRE(restored.config.tessellationDefaults.smoothAngle == 45);
    REQUIRE(restored.config.rules[0].tessellation->shading == config::Shading::Preserve);
    REQUIRE(restored.config.rules[0].tessellation->smoothAngle == 0);
    for (const auto value : {"-1", "181", "nan", "inf"}) {
        auto loaded = config::ConfigLoader::loadFromString(
            std::string("[defaults.tessellation]\nsmooth_angle = ") + value);
        REQUIRE(config::ConfigValidator::validate(loaded.config).hasErrors());
    }
    for (const auto value : {"\"invalid\"", "true", "12"}) {
        REQUIRE_THROWS(config::ConfigLoader::loadFromString(
            std::string("[defaults.tessellation]\nshading = ") + value));
    }
}

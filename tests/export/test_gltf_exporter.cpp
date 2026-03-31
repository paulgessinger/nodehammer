#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Include tinygltf for read-back (implementation lives in gltf_exporter.cpp).
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/export/gltf_exporter.hpp>
#include <nodehammer/export/obj_exporter.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <filesystem>
#include <fstream>

namespace {

// Build a RenderScene from a synthetic single-box scene with a default config.
nodehammer::RenderScene buildBoxRenderScene() {
    nodehammer::SemanticScene semScene = nodehammer::SyntheticSceneBuilder::buildSingleBox();
    nodehammer::NHConfig cfg;
    nodehammer::TessellationPass pass{cfg};
    return pass.lower(semScene).scene;
}

// Unique temp path to avoid test collisions.
std::filesystem::path tmpPath(const std::string &name) {
    return std::filesystem::temp_directory_path() / ("nh_test_" + name);
}

} // namespace

TEST_CASE("GltfExporter: single box → GLB magic bytes", "[export][gltf]") {
    const auto out = tmpPath("box.glb");

    nodehammer::GltfExporter exp;
    nodehammer::ExportConfig cfg;
    cfg.format = nodehammer::ExportConfig::Format::GLB;

    auto result = exp.write(buildBoxRenderScene(), out, cfg);
    CHECK_FALSE(result.diags.hasErrors());
    REQUIRE(std::filesystem::exists(out));

    // GLB magic = "glTF" = 0x46546C67 (little-endian)
    std::ifstream f{out, std::ios::binary};
    uint32_t magic{};
    f.read(reinterpret_cast<char *>(&magic), 4);
    CHECK(magic == 0x46546C67u);

    std::filesystem::remove(out);
}

TEST_CASE("GltfExporter: single box → node/mesh/material counts", "[export][gltf]") {
    const auto out = tmpPath("box_counts.glb");
    const auto scene = buildBoxRenderScene();

    nodehammer::GltfExporter exp;
    nodehammer::ExportConfig cfg;
    cfg.format = nodehammer::ExportConfig::Format::GLB;
    (void)exp.write(scene, out, cfg);

    tinygltf::TinyGLTF reader;
    tinygltf::Model model;
    std::string err, warn;
    REQUIRE(reader.LoadBinaryFromFile(&model, &err, &warn, out.string()));

    CHECK(model.nodes.size() == scene.nodes.size());
    CHECK(model.materials.size() == scene.materials.size());
    CHECK(model.meshes.size() >= 1);

    std::filesystem::remove(out);
}

TEST_CASE("GltfExporter: accessor byte offsets are 4-byte aligned", "[export][gltf]") {
    const auto out = tmpPath("box_align.glb");

    nodehammer::GltfExporter exp;
    nodehammer::ExportConfig cfg;
    cfg.format = nodehammer::ExportConfig::Format::GLB;
    (void)exp.write(buildBoxRenderScene(), out, cfg);

    tinygltf::TinyGLTF reader;
    tinygltf::Model model;
    std::string err, warn;
    REQUIRE(reader.LoadBinaryFromFile(&model, &err, &warn, out.string()));

    for (const auto &acc : model.accessors) {
        const std::size_t bvOffset = static_cast<std::size_t>(
            model.bufferViews[static_cast<std::size_t>(acc.bufferView)].byteOffset);
        const std::size_t accOffset = static_cast<std::size_t>(acc.byteOffset);
        CHECK((bvOffset + accOffset) % 4 == 0);
    }

    std::filesystem::remove(out);
}

TEST_CASE("GltfExporter: PBR material round-trip", "[export][gltf]") {
    const auto out = tmpPath("box_mat.glb");

    // Build scene with a specific material colour
    nodehammer::SemanticScene semScene = nodehammer::SyntheticSceneBuilder::buildSingleBox();
    nodehammer::NHConfig cfg;
    nodehammer::MaterialDef md;
    md.name = "red";
    md.baseColor = {1.0f, 0.0f, 0.0f, 1.0f};
    md.metallic = 0.2f;
    md.roughness = 0.8f;
    cfg.materials.push_back(md);
    nodehammer::MaterialRule mr;
    mr.materialName = "red";
    cfg.materialRules.push_back(mr);

    nodehammer::TessellationPass pass{cfg};
    const auto renderScene = pass.lower(semScene).scene;

    nodehammer::GltfExporter exp;
    nodehammer::ExportConfig ecfg;
    ecfg.format = nodehammer::ExportConfig::Format::GLB;
    (void)exp.write(renderScene, out, ecfg);

    tinygltf::TinyGLTF reader;
    tinygltf::Model model;
    std::string err, warn;
    REQUIRE(reader.LoadBinaryFromFile(&model, &err, &warn, out.string()));
    REQUIRE_FALSE(model.materials.empty());

    const auto &gm = model.materials.front();
    CHECK(gm.pbrMetallicRoughness.baseColorFactor[0] == Catch::Approx(1.0));
    CHECK(gm.pbrMetallicRoughness.baseColorFactor[1] == Catch::Approx(0.0));
    CHECK(gm.pbrMetallicRoughness.metallicFactor == Catch::Approx(0.2));
    CHECK(gm.pbrMetallicRoughness.roughnessFactor == Catch::Approx(0.8));

    std::filesystem::remove(out);
}

TEST_CASE("GltfExporter: write as GLTF (text)", "[export][gltf]") {
    const auto out = tmpPath("box.gltf");

    nodehammer::GltfExporter exp;
    nodehammer::ExportConfig cfg;
    cfg.format = nodehammer::ExportConfig::Format::GLTF;

    auto result = exp.write(buildBoxRenderScene(), out, cfg);
    CHECK_FALSE(result.diags.hasErrors());
    REQUIRE(std::filesystem::exists(out));

    // GLTF JSON starts with '{'
    std::ifstream f{out};
    char first{};
    f >> first;
    CHECK(first == '{');

    std::filesystem::remove(out);
}

TEST_CASE("ObjExporter: single box → OBJ + MTL files exist", "[export][obj]") {
    const auto out = tmpPath("box.obj");
    const auto mtl = tmpPath("box.mtl");

    nodehammer::ObjExporter exp;
    nodehammer::ExportConfig cfg;

    auto result = exp.write(buildBoxRenderScene(), out, cfg);
    CHECK_FALSE(result.diags.hasErrors());
    CHECK(std::filesystem::exists(out));
    CHECK(std::filesystem::exists(mtl));

    // OBJ should have at least v, vn, and f lines
    std::ifstream f{out};
    std::string line;
    bool hasV{}, hasVn{}, hasF{};
    while (std::getline(f, line)) {
        if (line.starts_with("v "))
            hasV = true;
        if (line.starts_with("vn "))
            hasVn = true;
        if (line.starts_with("f "))
            hasF = true;
    }
    CHECK(hasV);
    CHECK(hasVn);
    CHECK(hasF);

    std::filesystem::remove(out);
    std::filesystem::remove(mtl);
}

#include <CLI/CLI.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nodehammer/ir/render.hpp>
#include <print>
#include <string>

namespace {

nodehammer::RenderScene buildSyntheticRenderScene() {
    nodehammer::RenderScene scene;

    // Mesh asset: a box (6 faces × 4 verts = 24 verts, 12 tris = 36 indices)
    // Just a stub with 0 geometry for CP1 — real tessellation in CP6.
    auto meshId = scene.nextMeshId();
    nodehammer::MeshAsset mesh;
    mesh.id = meshId;
    mesh.name = "box_mesh";
    mesh.provenance.sourceSystem = "synthetic";
    mesh.provenance.sourceName = "boxLV";
    scene.meshAssets[meshId] = mesh;

    // Material
    auto matId = scene.nextMaterialId();
    nodehammer::RenderMaterial mat;
    mat.id = matId;
    mat.name = "aluminum";
    mat.baseColorFactor = glm::vec4{0.75f, 0.75f, 0.85f, 1.f};
    mat.metallicFactor = 0.1f;
    mat.roughnessFactor = 0.4f;
    scene.materials[matId] = mat;

    // Root render node
    auto nodeId = scene.nextNodeId();
    nodehammer::RenderNode node;
    node.id = nodeId;
    node.name = "world";
    node.semanticNodeId = nodehammer::SemanticNodeId{1};
    node.meshBindings.push_back({meshId, matId});
    scene.nodes[nodeId] = node;

    scene.rootId = nodeId;
    return scene;
}

} // namespace

void register_cmd_dump_render(CLI::App &app) {
    auto *sub = app.add_subcommand("dump-render", "Dump the render IR of a geometry as JSON");

    auto *input = sub->add_option("-i,--input", "Input geometry file");
    auto *input_format =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *config = sub->add_option("-c,--config", "TOML config file");
    auto *output = sub->add_option("-o,--output", "Output JSON file (default: stdout)");

    auto *syntheticBoxOpt =
        sub->add_flag("--synthetic-box", "Use a hardcoded synthetic box scene as input");

    (void)input;
    (void)input_format;
    (void)config;

    sub->callback([=] {
        if (!syntheticBoxOpt->count()) {
            std::println(
                stderr,
                "nodehammer dump-render: --synthetic-box is the only supported source in CP1");
            return;
        }

        auto scene = buildSyntheticRenderScene();
        nlohmann::json j = scene;
        std::string out_path;
        if (*output) {
            output->results(out_path);
            std::ofstream f{out_path};
            f << j.dump(2) << '\n';
        } else {
            std::println("{}", j.dump(2));
        }
    });
}

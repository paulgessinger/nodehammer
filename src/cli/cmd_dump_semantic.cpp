#include <CLI/CLI.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <print>
#include <string>

namespace {

nodehammer::SemanticScene buildSyntheticBox() {
    nodehammer::SemanticScene scene;

    // Shape
    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] =
        nodehammer::SemanticShape{shapeId, nodehammer::BoxShape{10.0, 10.0, 10.0}};

    // Material
    auto matId = scene.nextMaterialId();
    nodehammer::SourceMaterial mat;
    mat.id = matId;
    mat.name = "aluminum";
    mat.color = glm::vec3{0.75f, 0.75f, 0.85f};
    mat.density = 2.7;
    scene.materials[matId] = mat;

    // Logical volume
    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = nodehammer::SemanticLogicalVolume{lvId, "boxLV", shapeId, matId};

    // Root node
    auto nodeId = scene.nextNodeId();
    nodehammer::SemanticNode node;
    node.id = nodeId;
    node.name = "world";
    node.logVolId = lvId;
    node.provenance.sourceSystem = "synthetic";
    node.provenance.sourceName = "world";
    scene.nodes[nodeId] = node;

    scene.rootId = nodeId;
    scene.computeWorldTransforms();

    return scene;
}

} // namespace

void register_cmd_dump_semantic(CLI::App &app) {
    auto *sub = app.add_subcommand("dump-semantic", "Dump the semantic IR of a geometry as JSON");

    auto *input = sub->add_option("-i,--input", "Input geometry file");
    auto *input_format =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *output = sub->add_option("-o,--output", "Output JSON file (default: stdout)");

    auto *syntheticBoxOpt =
        sub->add_flag("--synthetic-box", "Use a hardcoded synthetic box scene as input");

    (void)input;
    (void)input_format;

    sub->callback([=] {
        if (!syntheticBoxOpt->count()) {
            std::println(
                stderr,
                "nodehammer dump-semantic: --synthetic-box is the only supported source in CP1");
            return;
        }

        auto scene = buildSyntheticBox();
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

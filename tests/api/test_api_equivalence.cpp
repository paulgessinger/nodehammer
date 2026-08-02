// #41's step-6 acceptance criterion: the public verbs produce byte-identical
// output to the internal pipeline the CLI drives.
//
// Both halves start from the same `.nhb` on disk and the same TOML text, then
// diverge — one through `SemanticScene::read` / `build` / `RenderScene::write`,
// the other through the importer, `SelectionEngine`, the dedup passes,
// `TessellationPass`, `resolveExportConfig` and the exporter, in the order
// `cmd_convert.cpp` runs them. The exported files are then compared byte for
// byte. A verb that reordered a stage, skipped dedup, or resolved export
// settings differently changes those bytes; nothing subtler than that is being
// claimed, and nothing weaker would catch the failure §3 warns about (a config
// honoured by `build` and ignored by `write`).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <api/handles.hpp>
#include <config/config_ast.hpp>
#include <config/config_loader.hpp>
#include <detail/file_io.hpp>
#include <export_resolve.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/render/exporter.hpp>
#include <ir/synthetic/semantic/importer.hpp>
#include <selection/selector.hpp>
#include <tessellation/tessellation_pass.hpp>

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace nh = nodehammer;

/// A config that touches every stage: selection prunes a node, dedup has
/// something to merge, a rule retargets a material and the circle segment
/// count, and both export tables carry a non-default unit scale. A verb that
/// dropped any one of these would move the exported bytes.
constexpr std::string_view kFullToml = R"(
hoist_orphans = false
deduplicate_shapes = true

[materials.shiny]
base_color = [0.1, 0.2, 0.3, 1.0]
metallic = 0.9
roughness = 0.1

[[selection_rules]]
[selection_rules.drop_if]
type = "name_glob"
pattern = "dropme"

[[rules]]
match = 'name ~= "tube"'
material = "shiny"
[rules.tessellation]
max_segments_circle = 24

[export.gltf]
unit_scale = 0.5
bake_unit_scale = true

[export.obj]
unit_scale = 0.25
)";

/// The same document with selection and dedup switched off, so the two stages
/// the standalone verbs make conditional are exercised in both states.
constexpr std::string_view kNoSelectionToml = R"(
deduplicate_shapes = false

[[rules]]
match = 'name ~= "tube"'
[rules.tessellation]
max_segments_circle = 12

[export.gltf]
unit_scale = 2.0
)";

/// A semantic scene with a tube (so `max_segments_circle` bites), a duplicate
/// box (so dedup has work), and a node the selection rule drops.
nh::ir::semantic::Scene sampleScene() {
    auto scene = nh::ir::SyntheticSceneBuilder::buildTubeInBox();

    // Two logical volumes with identical shape and material: dedup should merge
    // them, and the render output changes if it does not run.
    const auto makeBoxNode = [&](std::string_view name) {
        const auto shapeId = scene.nextShapeId();
        scene.shapes[shapeId] =
            nh::ir::semantic::Shape{shapeId, nh::ir::semantic::BoxShape{3.0, 3.0, 3.0}};
        const auto lvId = scene.nextLogVolId();
        scene.logVols[lvId] = nh::ir::semantic::LogicalVolume{
            lvId, std::string{name} + "LV", shapeId, nh::ir::semantic::MaterialId{1}};

        const auto nodeId = scene.nextNodeId();
        nh::ir::semantic::Node node;
        node.id = nodeId;
        node.name = std::string{name};
        node.logVolId = lvId;
        node.parentId = scene.rootId;
        node.sourceSystem = "synthetic";
        scene.nodes[nodeId] = std::move(node);
        scene.nodes[scene.rootId].children.push_back(nodeId);
    };

    makeBoxNode("boxA");
    makeBoxNode("boxB");
    makeBoxNode("dropme");

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

fs::path caseDir(std::string_view name) {
    const auto dir = fs::temp_directory_path() / "nh_api_equiv" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<std::byte> readBytes(const fs::path &path) {
    return nh::detail::file_io::readFile(path);
}

/// The internal pipeline, in `cmd_convert.cpp` order: select (only when rules
/// exist), dedup (only when enabled), tessellate, then resolve + export.
void referenceExport(const fs::path &nhb, const nh::config::NHConfig &cfg, const fs::path &out) {
    auto imported = nh::ir::FlatBufferImporter{}.import(nhb);
    REQUIRE_FALSE(imported.diags.hasErrors());
    auto scene = std::move(imported.scene);

    if (!cfg.selection.empty()) {
        const nh::selection::SelectionEngine engine{cfg.selection, cfg.hoistOrphans};
        const auto diags = engine.prune(scene);
        REQUIRE_FALSE(diags.hasErrors());
    }
    if (cfg.deduplicateShapes) {
        scene.deduplicateMaterials();
        scene.deduplicateShapes();
        scene.deduplicateLogVols();
    }

    const nh::tessellation::TessellationPass pass{cfg};
    auto lowered = pass.lower(scene);
    REQUIRE_FALSE(lowered.diags.hasErrors());

    const auto registry = nh::ir::RenderExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(out, {});
    REQUIRE(exporter != nullptr);
    const auto resolved = nh::pipeline::resolveExportConfig(cfg, out, {});
    const auto result = exporter->write(lowered.scene, out, resolved);
    REQUIRE_FALSE(result.diags.hasErrors());
}

} // namespace

TEST_CASE("Public verbs export byte-identically to the internal pipeline", "[api][equivalence]") {
    const auto toml = GENERATE(kFullToml, kNoSelectionToml);
    const auto extension =
        GENERATE(std::string_view{".glb"}, std::string_view{".gltf"}, std::string_view{".obj"});
    CAPTURE(toml, extension);

    const auto dir = caseDir("export");
    const auto nhb = dir / "scene.nhb";
    nh::detail::file_io::writeFile(nhb, nh::ir::semanticSceneToBytes(sampleScene()));

    // Same basename, different directories. OBJ writes `mtllib <stem>.mtl` and
    // glTF a `<stem>.bin` URI into the file itself, so comparing two outputs
    // that differ only in name would fail on the name.
    fs::create_directories(dir / "api");
    fs::create_directories(dir / "reference");

    // ── The reference: internal types throughout ─────────────────────────────
    const auto loaded = nh::config::ConfigLoader::loadFromString(toml, "<string>", dir);
    REQUIRE_FALSE(loaded.diags.hasErrors());
    const auto reference = dir / "reference" / ("out" + std::string{extension});
    referenceExport(nhb, loaded.config, reference);

    // ── The public surface: handles throughout ───────────────────────────────
    const auto cfg = nh::Config::parse(toml, dir);
    REQUIRE_FALSE(cfg.diags.hasErrors());

    const auto sem = nh::SemanticScene::read(nhb);
    REQUIRE_FALSE(sem.diags.hasErrors());
    REQUIRE(sem.scene.valid());

    const auto rendered = nh::build(sem.scene, cfg.config.scene());
    REQUIRE_FALSE(rendered.diags.hasErrors());
    REQUIRE(rendered.scene.valid());

    const auto viaApi = dir / "api" / ("out" + std::string{extension});
    const auto writeDiags = rendered.scene.write(viaApi, cfg.config.output());
    REQUIRE_FALSE(writeDiags.hasErrors());

    REQUIRE(readBytes(viaApi) == readBytes(reference));

    // OBJ writes a sidecar .mtl and glTF a sidecar .bin; both are part of the
    // output and neither is covered by comparing the primary file alone.
    for (const auto &sidecar : {".mtl", ".bin"}) {
        const auto a = viaApi.parent_path() / (viaApi.stem().string() + sidecar);
        const auto b = reference.parent_path() / (reference.stem().string() + sidecar);
        if (fs::exists(a) || fs::exists(b)) {
            REQUIRE(fs::exists(a));
            REQUIRE(fs::exists(b));
            REQUIRE(readBytes(a) == readBytes(b));
        }
    }
}

TEST_CASE("build equals applySelection + deduplicate + tessellate", "[api][equivalence]") {
    // §8's gap, as a test: the decomposition has to reproduce the whole. If
    // `deduplicate` were missing from the surface, or `build` ran the stages in
    // a different order, these two `.nhr` blobs would differ.
    const auto dir = caseDir("decompose");
    const auto nhb = dir / "scene.nhb";
    nh::detail::file_io::writeFile(nhb, nh::ir::semanticSceneToBytes(sampleScene()));

    const auto cfg = nh::Config::parse(kFullToml, dir);
    REQUIRE_FALSE(cfg.diags.hasErrors());
    const auto scene = cfg.config.scene();

    const auto sem = nh::SemanticScene::read(nhb);
    REQUIRE(sem.scene.valid());

    const auto whole = nh::build(sem.scene, scene);
    REQUIRE(whole.scene.valid());

    const auto selected = nh::applySelection(sem.scene, scene);
    REQUIRE(selected.scene.valid());
    // The selection rule has to actually bite, or this proves nothing.
    REQUIRE(selected.scene.nodeCount() < sem.scene.nodeCount());

    const auto deduped = nh::deduplicate(selected.scene, scene);
    REQUIRE(deduped.scene.valid());
    REQUIRE(deduped.scene.logVolCount() < selected.scene.logVolCount());

    const auto piecewise = nh::tessellate(deduped.scene, scene);
    REQUIRE(piecewise.scene.valid());

    REQUIRE(whole.scene.toNhr() == piecewise.scene.toNhr());

    // And the counterexample the verb exists to prevent: skipping `deduplicate`
    // gives a different scene, silently, with no diagnostic to say so.
    const auto withoutDedup = nh::tessellate(selected.scene, scene);
    REQUIRE(withoutDedup.scene.valid());
    REQUIRE(withoutDedup.scene.toNhr() != whole.scene.toNhr());
}

TEST_CASE("The stage verbs are no-ops when their config switch is off", "[api][equivalence]") {
    const auto dir = caseDir("switches");
    const auto nhb = dir / "scene.nhb";
    nh::detail::file_io::writeFile(nhb, nh::ir::semanticSceneToBytes(sampleScene()));

    const auto cfg = nh::Config::parse(kNoSelectionToml, dir);
    REQUIRE_FALSE(cfg.diags.hasErrors());
    const auto sem = nh::SemanticScene::read(nhb);
    REQUIRE(sem.scene.valid());

    // No selection rules means no filtering — not "keep everything", which
    // would still garbage-collect unreferenced volumes.
    const auto selected = nh::applySelection(sem.scene, cfg.config.scene());
    REQUIRE(selected.scene.nodeCount() == sem.scene.nodeCount());
    REQUIRE(selected.scene.logVolCount() == sem.scene.logVolCount());

    // `deduplicate_shapes = false` makes the dedup verb a no-op even though
    // this scene has duplicates to merge.
    const auto deduped = nh::deduplicate(sem.scene, cfg.config.scene());
    REQUIRE(deduped.scene.logVolCount() == sem.scene.logVolCount());
}

TEST_CASE("write honours the output slice, not just build", "[api][equivalence]") {
    // The concrete bug §3 describes: a config sets `[export.gltf] unit_scale`,
    // `build` honours it, and `write` does not. Passing a default slice must
    // produce different bytes from passing the document's own.
    const auto dir = caseDir("unit_scale");
    const auto nhb = dir / "scene.nhb";
    nh::detail::file_io::writeFile(nhb, nh::ir::semanticSceneToBytes(sampleScene()));

    const auto cfg = nh::Config::parse(kFullToml, dir);
    const auto sem = nh::SemanticScene::read(nhb);
    const auto rendered = nh::build(sem.scene, cfg.config.scene());
    REQUIRE(rendered.scene.valid());

    const auto tuned = dir / "tuned.glb";
    const auto defaulted = dir / "defaulted.glb";
    REQUIRE_FALSE(rendered.scene.write(tuned, cfg.config.output()).hasErrors());
    REQUIRE_FALSE(rendered.scene.write(defaulted).hasErrors());
    REQUIRE(readBytes(tuned) != readBytes(defaulted));

    // And an omitted slice means exactly the format's built-in defaults —
    // pinned against the same resolution the CLI runs, over the same render
    // scene, so this compares the write path and nothing else.
    REQUIRE(rendered.scene.valid());
    const auto &internalScene = rendered.scene.impl().scene;
    const auto registry = nh::ir::RenderExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(defaulted, {});
    REQUIRE(exporter != nullptr);
    const auto expected = dir / "expected.glb";
    const nh::config::NHConfig noConfig;
    REQUIRE_FALSE(
        exporter
            ->write(internalScene, expected, nh::pipeline::resolveExportConfig(noConfig, expected))
            .diags.hasErrors());
    REQUIRE(readBytes(defaulted) == readBytes(expected));
}

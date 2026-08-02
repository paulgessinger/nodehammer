// Cross-toolchain coverage for the compute-worker pipeline body.
//
// nh_compute_build (src/web/compute_worker_main.cpp) is the C/EM_JS shell that
// only compiles under Emscripten, but everything it actually does lives in
// nodehammer_lib. This test exercises that exact sequence — deserialize .nhb,
// parse flat TOML, prep, (optional) wedge cut, tessellate, serialize .nhr,
// deserialize .nhr — so the boundary logic is verified on native *and* under
// wasm/node, independent of the worker glue.

#include <catch2/catch_test_macros.hpp>

#include <config/config_loader.hpp>
#include <ir/fb/render/flatbuffer.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/synthetic/semantic/importer.hpp>
#include <scene_build.hpp>
#include <tessellation/tessellation_job.hpp>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::pipeline;
using namespace nodehammer::tessellation;
using namespace nodehammer::config;

namespace {

// Mirror of nh_compute_build's body: bytes in -> render bytes out.
std::vector<std::byte> runComputePipeline(std::span<const std::byte> sceneBytes,
                                          std::string_view configToml,
                                          std::optional<WedgeCutParams> wedge) {
    auto imported = FlatBufferImporter::importFromBytes("scene.nhb", sceneBytes);
    REQUIRE_FALSE(imported.diags.hasErrors());

    auto loaded = ConfigLoader::loadFromString(configToml, "<worker-config>");
    REQUIRE_FALSE(loaded.diags.hasErrors());

    ScenePrepResult prep = prepareSceneForTessellationFromInputs(
        std::move(loaded.config), std::move(imported.scene), std::nullopt);

    if (wedge) {
        WedgeCutJob job;
        job.start(prep.scene, *wedge);
        while (!job.advance(std::numeric_limits<std::uint64_t>::max())) {
        }
        (void)job.take();
    }

    TessellationJob tess;
    tess.start(prep.config, prep.scene);
    while (!tess.advance(std::numeric_limits<std::uint64_t>::max())) {
    }
    TessellationPassResult result = tess.take();
    REQUIRE_FALSE(result.diags.hasErrors());

    return renderSceneToBytes(result.scene);
}

} // namespace

TEST_CASE("Compute pipeline: nested boxes -> NHR8 render bytes", "[compute][pipeline]") {
    auto sceneBytes = semanticSceneToBytes(SyntheticSceneBuilder::buildNestedBoxes());

    auto renderBytes = runComputePipeline(std::as_bytes(std::span{sceneBytes}), "", std::nullopt);
    REQUIRE(!renderBytes.empty());

    // The bytes the worker would transfer back deserialize to a usable scene.
    auto render = renderSceneFromBytes(std::as_bytes(std::span{renderBytes}));
    REQUIRE(render.nodes.contains(render.rootId));
    REQUIRE(!render.meshAssets.empty());
    // Each mesh asset carries real geometry.
    for (const auto &[id, mesh] : render.meshAssets) {
        REQUIRE(!mesh.vertices.empty());
        REQUIRE(!mesh.indices.empty());
        REQUIRE(mesh.indices.size() % 3 == 0);
    }
}

TEST_CASE("Compute pipeline: wedge cut path runs end-to-end", "[compute][pipeline]") {
    auto sceneBytes = semanticSceneToBytes(SyntheticSceneBuilder::buildNestedBoxes());

    // Remove the first quadrant. We don't assert cut specifics (covered by the
    // wedge-cut tests) — only that the has_wedge branch completes and yields a
    // valid, round-trippable render scene.
    WedgeCutParams wedge{.startDeg = 0.0, .endDeg = 90.0, .margin = 2.0};
    auto renderBytes = runComputePipeline(std::as_bytes(std::span{sceneBytes}), "", wedge);
    REQUIRE(!renderBytes.empty());

    auto render = renderSceneFromBytes(std::as_bytes(std::span{renderBytes}));
    REQUIRE(render.nodes.contains(render.rootId));
}

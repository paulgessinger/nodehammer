// Unit coverage for BuildPipeline — the core primitive that unifies the four
// hand-copied `prep → wedge → tessellate` build sequences (native worker,
// web-cooperative, web-worker, synchronous). Because BuildPipeline lives in
// nodehammer_lib with no viewer/GPU deps, the whole state machine is testable
// here on native and under wasm/node.
//
// The tests lock the invariants the four backends used to enforce only by
// discipline: drive-to-completion parity with the one-shot TessellationPass,
// wedge parity with the pre-refactor synchronous path, budget slicing yielding
// an identical scene, phase/counter progression, error propagation, and clean
// handling of a degenerate/absent wedge.

#include <catch2/catch_test_macros.hpp>

#include <nodehammer/ir/synthetic/semantic/importer.hpp>
#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/build_pipeline.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

using namespace nodehammer;

namespace {

constexpr std::uint64_t kSpin = std::numeric_limits<std::uint64_t>::max();

// A structural fingerprint of a RenderScene that is stable under different
// RenderNodeId/MeshAssetId allocation order but captures node topology, mesh
// geometry, and the node→mesh mapping — enough to assert two scenes are "the
// same build".
struct SceneFingerprint {
    std::size_t nodeCount{};
    std::size_t meshCount{};
    std::size_t materialCount{};
    std::size_t totalVertices{};
    std::size_t totalIndices{};
    // Sorted (vertexCount, indexCount) per mesh asset — order-independent.
    std::vector<std::pair<std::size_t, std::size_t>> meshShapes;
    // semanticNodeId → number of mesh bindings on the render node it produced.
    std::map<std::uint64_t, std::size_t> bindingsBySemanticNode;

    bool operator==(const SceneFingerprint &) const = default;
};

SceneFingerprint fingerprint(const detail::RenderScene &sc) {
    SceneFingerprint fp;
    fp.nodeCount = sc.nodes.size();
    fp.meshCount = sc.meshAssets.size();
    fp.materialCount = sc.materials.size();
    for (const auto &[id, mesh] : sc.meshAssets) {
        fp.totalVertices += mesh.vertices.size();
        fp.totalIndices += mesh.indices.size();
        fp.meshShapes.emplace_back(mesh.vertices.size(), mesh.indices.size());
    }
    std::sort(fp.meshShapes.begin(), fp.meshShapes.end());
    for (const auto &[id, node] : sc.nodes) {
        fp.bindingsBySemanticNode[node.semanticNodeId.value] = node.meshBindings.size();
    }
    return fp;
}

std::shared_ptr<const NHConfig> emptyConfig() { return std::make_shared<const NHConfig>(); }

// The pre-refactor synchronous reference: prep (with an inline wedge, matching
// the old buildSceneFromPaths / convert ordering) then a one-shot lower().
detail::RenderScene referenceBuild(const detail::SemanticScene &scene,
                                   std::optional<WedgeCutParams> wedge) {
    ScenePrepResult prep = prepareSceneForTessellationFromInputs(NHConfig{}, scene, wedge);
    REQUIRE(prep.ok);
    TessellationPass pass{prep.config};
    TessellationPassResult tess = pass.lower(prep.scene);
    REQUIRE_FALSE(tess.diags.hasErrors());
    return std::move(tess.scene);
}

// Drive a fresh pipeline to completion with the given per-slice budget.
SceneBuildResult drivePipeline(const detail::SemanticScene &scene,
                               std::optional<WedgeCutParams> wedge, std::uint64_t budget) {
    BuildPipeline pipe;
    pipe.start(emptyConfig(), std::make_shared<const detail::SemanticScene>(scene), wedge);
    while (!pipe.advance(budget)) {
    }
    return pipe.take();
}

} // namespace

TEST_CASE("BuildPipeline: drive-to-completion parity with one-shot lower", "[build_pipeline]") {
    const detail::SemanticScene scene = SyntheticSceneBuilder::buildNestedBoxes();

    SceneBuildResult built = drivePipeline(scene, std::nullopt, kSpin);
    REQUIRE_FALSE(built.diags.hasErrors());
    REQUIRE(built.scene != nullptr);

    const detail::RenderScene reference = referenceBuild(scene, std::nullopt);
    REQUIRE(fingerprint(*built.scene) == fingerprint(reference));
}

TEST_CASE("BuildPipeline: wedge parity with the pre-refactor synchronous path",
          "[build_pipeline]") {
    const detail::SemanticScene scene = SyntheticSceneBuilder::buildNestedBoxes();
    const WedgeCutParams wedge{.startDeg = 0.0, .endDeg = 90.0, .margin = 2.0};

    SceneBuildResult built = drivePipeline(scene, wedge, kSpin);
    REQUIRE_FALSE(built.diags.hasErrors());
    REQUIRE(built.scene != nullptr);

    // Reference applies the wedge inline in prep; the pipeline defers it to a
    // WedgeCutJob. wedge_cut.hpp documents applyWedgeCut as a thin shim over
    // WedgeCutJob, so the geometry must match.
    const detail::RenderScene reference = referenceBuild(scene, wedge);
    REQUIRE(fingerprint(*built.scene) == fingerprint(reference));
}

TEST_CASE("BuildPipeline: budget slicing yields an identical scene", "[build_pipeline]") {
    const detail::SemanticScene scene = SyntheticSceneBuilder::buildNestedBoxes();

    // A tiny budget forces many advance() iterations.
    BuildPipeline pipe;
    pipe.start(emptyConfig(), std::make_shared<const detail::SemanticScene>(scene), std::nullopt);
    int falses = 0;
    while (!pipe.advance(1 /* ns */)) {
        ++falses;
        REQUIRE(falses < 100000); // guard against a stuck machine
    }
    SceneBuildResult sliced = pipe.take();
    REQUIRE_FALSE(sliced.diags.hasErrors());
    REQUIRE(sliced.scene != nullptr);

    // At minimum: the Queued→Preparing burn plus the Preparing slice → ≥ 2
    // false returns before completion.
    REQUIRE(falses >= 2);

    const detail::RenderScene reference = referenceBuild(scene, std::nullopt);
    REQUIRE(fingerprint(*sliced.scene) == fingerprint(reference));
}

TEST_CASE("BuildPipeline: phase and counter progression", "[build_pipeline]") {
    const detail::SemanticScene scene = SyntheticSceneBuilder::buildNestedBoxes();
    const WedgeCutParams wedge{.startDeg = 0.0, .endDeg = 90.0, .margin = 2.0};

    BuildPipeline pipe;
    REQUIRE(pipe.phase() == BuildPipeline::Phase::Idle);
    pipe.start(emptyConfig(), std::make_shared<const detail::SemanticScene>(scene), wedge);
    REQUIRE(pipe.phase() == BuildPipeline::Phase::Queued);

    // Counters are 0 before their phases run.
    REQUIRE(pipe.wedgeCutProcessed() == 0);
    REQUIRE(pipe.tessellationProcessed() == 0);

    std::vector<BuildPipeline::Phase> seen;
    seen.push_back(pipe.phase());
    // Tiny budget so we observe each intermediate phase.
    while (!pipe.advance(1)) {
        if (seen.empty() || seen.back() != pipe.phase()) {
            seen.push_back(pipe.phase());
        }
    }
    seen.push_back(pipe.phase());

    // Every phase in canonical order appears, monotonically.
    const std::vector<BuildPipeline::Phase> expected{
        BuildPipeline::Phase::Queued,     BuildPipeline::Phase::Preparing,
        BuildPipeline::Phase::Cutting,    BuildPipeline::Phase::Tessellating,
        BuildPipeline::Phase::Finalizing, BuildPipeline::Phase::Done,
    };
    // Filter `seen` down to first-occurrence order and compare.
    std::vector<BuildPipeline::Phase> firsts;
    for (auto p : seen) {
        if (firsts.empty() || firsts.back() != p) {
            firsts.push_back(p);
        }
    }
    // `firsts` must be a subsequence of the canonical order, not necessarily
    // equal to it: WedgeCutJob/TessellationJob only sample the clock every
    // kClockCheckStride items (coarse-worker-clock guard), so a phase whose
    // total work is smaller than that stride can run to completion inside a
    // single advance() call and never surface as its own observed state.
    auto expected_it = expected.begin();
    for (auto p : firsts) {
        expected_it = std::find(expected_it, expected.end(), p);
        REQUIRE(expected_it != expected.end());
        ++expected_it;
    }

    // Each processed reached its total.
    REQUIRE(pipe.wedgeCutTotal() > 0);
    REQUIRE(pipe.wedgeCutProcessed() == pipe.wedgeCutTotal());
    REQUIRE(pipe.tessellationTotal() > 0);
    REQUIRE(pipe.tessellationProcessed() == pipe.tessellationTotal());

    SceneBuildResult r = pipe.take();
    REQUIRE(r.scene != nullptr);
    REQUIRE(pipe.phase() == BuildPipeline::Phase::Idle); // take() resets
}

TEST_CASE("BuildPipeline: error propagation takes the failure path", "[build_pipeline]") {
    // A rule referencing an undefined material fails ConfigValidator, so prep
    // returns !ok and the pipeline lands on the failure branch.
    detail::SemanticScene scene = SyntheticSceneBuilder::buildSingleBox();
    NHConfig cfg;
    Rule rule;
    rule.material = "does_not_exist";
    cfg.rules.push_back(std::move(rule));

    BuildPipeline pipe;
    pipe.start(std::make_shared<const NHConfig>(std::move(cfg)),
               std::make_shared<const detail::SemanticScene>(std::move(scene)), std::nullopt);
    while (!pipe.advance(kSpin)) {
    }
    SceneBuildResult r = pipe.take();
    REQUIRE(r.scene == nullptr);
    REQUIRE(r.diags.hasErrors());
}

TEST_CASE("BuildPipeline: degenerate and absent wedge skip Cutting", "[build_pipeline]") {
    const detail::SemanticScene scene = SyntheticSceneBuilder::buildNestedBoxes();
    const detail::RenderScene reference = referenceBuild(scene, std::nullopt);

    SECTION("absent wedge") {
        SceneBuildResult r = drivePipeline(scene, std::nullopt, kSpin);
        REQUIRE(r.scene != nullptr);
        REQUIRE(fingerprint(*r.scene) == fingerprint(reference));
    }

    SECTION("degenerate ~0deg sector") {
        // A ≈0° removed sector is a no-op cut: the scene tessellates unchanged.
        const WedgeCutParams wedge{.startDeg = 0.0, .endDeg = 0.0, .margin = 2.0};
        SceneBuildResult r = drivePipeline(scene, wedge, kSpin);
        REQUIRE(r.scene != nullptr);
        REQUIRE(fingerprint(*r.scene) == fingerprint(reference));
    }
}

#include <api/handles.hpp>

#include <diagnostic_codes.hpp>
#include <selection/selector.hpp>
#include <tessellation/tessellation_pass.hpp>

#include <exception>
#include <format>
#include <utility>

namespace nodehammer {
namespace {

/// The stage conditions, written once so the standalone verbs and `build`
/// cannot disagree about them. Both mirror `convert`: no selection rules means
/// no filtering (rather than "keep everything", which would still garbage-
/// collect), and dedup honours the flag that turns it off.
[[nodiscard]] bool selectionApplies(const config::NHConfig &cfg) { return !cfg.selection.empty(); }

[[nodiscard]] bool dedupApplies(const config::NHConfig &cfg) { return cfg.deduplicateShapes; }

void runSelection(ir::semantic::Scene &scene, const config::NHConfig &cfg,
                  diagnostics::List &diags) {
    const selection::SelectionEngine engine{cfg.selection, cfg.hoistOrphans};
    diags.append(engine.prune(scene));
}

/// Deduplicate, and say how much it merged.
///
/// The counts were being computed and dropped: the CLI printed them and the API
/// discarded them, so a caller had no way to tell "dedup ran" from "dedup did
/// something". They are `Info` — the result is exactly what was asked for, and
/// this is worth recording (docs/error-model.md).
void runDedup(ir::semantic::Scene &scene, diagnostics::List &diags) {
    const auto materials = scene.deduplicateMaterials();
    const auto shapes = scene.deduplicateShapes();
    const auto logVols = scene.deduplicateLogVols();
    if (materials == 0 && shapes == 0 && logVols == 0) {
        return;
    }
    diags.info(codes::kInfoDedupMerged,
               std::format("deduplication merged {} shapes, {} logical volumes and {} materials",
                           shapes, logVols, materials));
}

} // namespace

SemanticResult applySelection(const SemanticScene &scene, const SceneConfig &config) {
    const auto &input = api::sceneOrThrow(scene, "applySelection");
    const auto &cfg = api::documentOf(config);
    if (!selectionApplies(cfg)) {
        return SemanticResult{scene, DiagnosticList{}};
    }

    ir::semantic::Scene working = input;
    diagnostics::List diags;
    runSelection(working, cfg, diags);
    // The scene comes back even when the diagnostics carry errors — a
    // root-dropped rule leaves `prune` a no-op and says so (NH0401), and
    // handing back nothing would hide the scene the caller still has.
    return SemanticResult{api::asHandle(std::move(working)),
                          diagnostics::asHandle(std::move(diags))};
}

SemanticResult deduplicate(const SemanticScene &scene, const SceneConfig &config) {
    const auto &input = api::sceneOrThrow(scene, "deduplicate");
    const auto &cfg = api::documentOf(config);
    if (!dedupApplies(cfg)) {
        return SemanticResult{scene, DiagnosticList{}};
    }

    ir::semantic::Scene working = input;
    diagnostics::List diags;
    runDedup(working, diags);
    return SemanticResult{api::asHandle(std::move(working)),
                          diagnostics::asHandle(std::move(diags))};
}

RenderResult tessellate(const SemanticScene &scene, const SceneConfig &config) {
    const auto &input = api::sceneOrThrow(scene, "tessellate");
    const auto &cfg = api::documentOf(config);
    const tessellation::TessellationPass pass{cfg};
    auto result = pass.lower(input);
    // Errors and a usable scene genuinely coexist here: NH0500 reports an
    // unknown shape and leaves that one node without a mesh binding, and the
    // rest of the scene is still a scene. Returning it is what lets a caller
    // decide whether to export anyway, exactly as the internal pass allows.
    return RenderResult{api::asHandle(std::move(result.scene)),
                        diagnostics::asHandle(std::move(result.diags))};
}

RenderResult build(const SemanticScene &scene, const SceneConfig &config) {
    const auto &input = api::sceneOrThrow(scene, "build");
    const auto &cfg = api::documentOf(config);

    // One working copy for all three stages rather than three handles chained
    // through the public verbs: same order, same conditions, one copy of the
    // scene instead of three.
    ir::semantic::Scene working = input;
    diagnostics::List diags;

    if (selectionApplies(cfg)) {
        runSelection(working, cfg, diags);
    }
    if (dedupApplies(cfg)) {
        runDedup(working, diags);
    }

    const tessellation::TessellationPass pass{cfg};
    auto result = pass.lower(working);
    diags.append(result.diags);
    // Tessellation errors come back *with* the scene — see `tessellate`.
    return RenderResult{api::asHandle(std::move(result.scene)),
                        diagnostics::asHandle(std::move(diags))};
}

} // namespace nodehammer

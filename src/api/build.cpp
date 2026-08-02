#include <api/handles.hpp>

#include <diagnostic_codes.hpp>
#include <selection/selector.hpp>
#include <tessellation/tessellation_pass.hpp>

#include <exception>
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

void runDedup(ir::semantic::Scene &scene) {
    scene.deduplicateMaterials();
    scene.deduplicateShapes();
    scene.deduplicateLogVols();
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
    return SemanticResult{api::asHandle(std::move(working)), api::asHandle(std::move(diags))};
}

SemanticResult deduplicate(const SemanticScene &scene, const SceneConfig &config) {
    const auto &input = api::sceneOrThrow(scene, "deduplicate");
    const auto &cfg = api::documentOf(config);
    if (!dedupApplies(cfg)) {
        return SemanticResult{scene, DiagnosticList{}};
    }

    ir::semantic::Scene working = input;
    runDedup(working);
    return SemanticResult{api::asHandle(std::move(working)), DiagnosticList{}};
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
                        api::asHandle(std::move(result.diags))};
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
        // Selection failing is the one stage whose failure leaves nothing to
        // hand back: there is no render scene to attach the reason to, because
        // tessellation has not run and running it on a scene the rules meant to
        // prune would be worse than stopping. `convert` stops here too.
        if (diags.hasErrors()) {
            throw Error{codes::kFatalSelectionRootDropped, diags.items().front().message, "build"};
        }
    }
    if (dedupApplies(cfg)) {
        runDedup(working);
    }

    const tessellation::TessellationPass pass{cfg};
    auto result = pass.lower(working);
    diags.append(result.diags);
    // Tessellation errors come back *with* the scene — see `tessellate`.
    return RenderResult{api::asHandle(std::move(result.scene)), api::asHandle(std::move(diags))};
}

} // namespace nodehammer

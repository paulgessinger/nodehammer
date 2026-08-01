#include <api/handles.hpp>

#include <diagnostic_codes.hpp>
#include <selection/selector.hpp>
#include <tessellation/tessellation_pass.hpp>

#include <exception>
#include <utility>

namespace nodehammer {
namespace {

using api::Access;

/// The stage conditions, written once so the standalone verbs and `build`
/// cannot disagree about them. Both mirror `convert`: no selection rules means
/// no filtering (rather than "keep everything", which would still garbage-
/// collect), and dedup honours the flag that turns it off.
[[nodiscard]] bool selectionApplies(const config::NHConfig &cfg) { return !cfg.selection.empty(); }

[[nodiscard]] bool dedupApplies(const config::NHConfig &cfg) { return cfg.deduplicateShapes; }

void runSelection(ir::semantic::Scene &scene, const config::NHConfig &cfg,
                  diagnostics::DiagnosticList &diags) {
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
    const auto *input = Access::sceneOf(scene);
    if (input == nullptr) {
        return SemanticResult{SemanticScene{},
                              Access::error(codes::kErrApiInvalidHandle,
                                            "applySelection: the scene handle refers to nothing")};
    }
    const auto &cfg = Access::configOf(config);
    if (!selectionApplies(cfg)) {
        return SemanticResult{scene, DiagnosticList{}};
    }

    try {
        ir::semantic::Scene working = *input;
        diagnostics::DiagnosticList diags;
        runSelection(working, cfg, diags);
        if (diags.hasErrors()) {
            return SemanticResult{SemanticScene{}, Access::wrap(diags)};
        }
        return SemanticResult{Access::wrap(std::move(working)), Access::wrap(diags)};
    } catch (const std::exception &e) {
        return SemanticResult{SemanticScene{},
                              Access::error(codes::kErrSelectionRootDropped, e.what())};
    }
}

SemanticResult deduplicate(const SemanticScene &scene, const SceneConfig &config) {
    const auto *input = Access::sceneOf(scene);
    if (input == nullptr) {
        return SemanticResult{SemanticScene{},
                              Access::error(codes::kErrApiInvalidHandle,
                                            "deduplicate: the scene handle refers to nothing")};
    }
    const auto &cfg = Access::configOf(config);
    if (!dedupApplies(cfg)) {
        return SemanticResult{scene, DiagnosticList{}};
    }

    ir::semantic::Scene working = *input;
    runDedup(working);
    return SemanticResult{Access::wrap(std::move(working)), DiagnosticList{}};
}

RenderResult tessellate(const SemanticScene &scene, const SceneConfig &config) {
    const auto *input = Access::sceneOf(scene);
    if (input == nullptr) {
        return RenderResult{RenderScene{},
                            Access::error(codes::kErrApiInvalidHandle,
                                          "tessellate: the scene handle refers to nothing")};
    }
    const auto &cfg = Access::configOf(config);
    try {
        const tessellation::TessellationPass pass{cfg};
        auto result = pass.lower(*input);
        if (result.diags.hasErrors()) {
            return RenderResult{RenderScene{}, Access::wrap(result.diags)};
        }
        return RenderResult{Access::wrap(std::move(result.scene)), Access::wrap(result.diags)};
    } catch (const std::exception &e) {
        return RenderResult{RenderScene{}, Access::error(codes::kErrTessBooleanFail, e.what())};
    }
}

RenderResult build(const SemanticScene &scene, const SceneConfig &config) {
    const auto *input = Access::sceneOf(scene);
    if (input == nullptr) {
        return RenderResult{RenderScene{}, Access::error(codes::kErrApiInvalidHandle,
                                                         "build: the scene handle refers to "
                                                         "nothing")};
    }
    const auto &cfg = Access::configOf(config);

    // One working copy for all three stages rather than three handles chained
    // through the public verbs: same order, same conditions, one copy of the
    // scene instead of three.
    try {
        ir::semantic::Scene working = *input;
        diagnostics::DiagnosticList diags;

        if (selectionApplies(cfg)) {
            runSelection(working, cfg, diags);
            if (diags.hasErrors()) {
                return RenderResult{RenderScene{}, Access::wrap(diags)};
            }
        }
        if (dedupApplies(cfg)) {
            runDedup(working);
        }

        const tessellation::TessellationPass pass{cfg};
        auto result = pass.lower(working);
        diags.append(result.diags);
        if (diags.hasErrors()) {
            return RenderResult{RenderScene{}, Access::wrap(diags)};
        }
        return RenderResult{Access::wrap(std::move(result.scene)), Access::wrap(diags)};
    } catch (const std::exception &e) {
        return RenderResult{RenderScene{}, Access::error(codes::kErrTessBooleanFail, e.what())};
    }
}

} // namespace nodehammer

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

// `SelectionEngine` takes its rules by value, so constructing one copies a
// `vector<SelectionRule>`, and copying a `SelectionRule` copies a `PredicateExpr`
// — a `std::variant` holding `shared_ptr`s for the compound predicates. At -O3,
// GCC 15 inlines the variant's copy constructor together with the destructor
// that only ever runs if that construction throws, then reports the
// control-block pointer on that unreachable path as maybe-uninitialized. It is
// the known false positive for `variant` + `shared_ptr`, and `-Werror` makes it
// fatal on the LCG job — the one configuration that compiles with this gcc.
//
// Scoped to this one function on purpose, so it cannot mask a real report
// elsewhere in the file, and guarded because the option does not exist under
// clang. `cmd_convert.cpp` writes the identical construction and does not trip
// it; what differs is only how much this small helper gets inlined into its two
// callers, which is why suppressing beats restructuring around it.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
void runSelection(ir::semantic::Scene &scene, const config::NHConfig &cfg,
                  diagnostics::DiagnosticList &diags) {
    const selection::SelectionEngine engine{cfg.selection, cfg.hoistOrphans};
    diags.append(engine.prune(scene));
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

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
        // The scene comes back even when the diagnostics carry errors — a
        // root-dropped rule leaves `prune` a no-op and says so (NH0401), and
        // handing back nothing would hide the scene the caller still has.
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
        // Errors and a usable scene genuinely coexist here: NH0500 reports an
        // unknown shape and leaves that one node without a mesh binding, and the
        // rest of the scene is still a scene. Returning it is what lets a caller
        // decide whether to export anyway, exactly as the internal pass allows.
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
            // The only place this function can return nothing: a failed
            // selection has produced no render scene *yet*, so there is nothing
            // to hand back but the reason. Stopping here rather than
            // tessellating a scene the rules meant to prune is what `convert`
            // does too.
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
        // Tessellation errors come back *with* the scene — see `tessellate`.
        return RenderResult{Access::wrap(std::move(result.scene)), Access::wrap(diags)};
    } catch (const std::exception &e) {
        return RenderResult{RenderScene{}, Access::error(codes::kErrTessBooleanFail, e.what())};
    }
}

} // namespace nodehammer

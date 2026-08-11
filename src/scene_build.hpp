#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/render.hpp>
#include <ir/semantic.hpp>
#include <tessellation/wedge_cut.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace nodehammer::pipeline {

/// Outcome of an asynchronous build.
///
/// The one place a fatal failure is a *value* rather than an exception: it
/// arrives from another thread of control — a worker thread, a `postMessage`
/// callback — where there is no call left to unwind. So the exception channel is
/// materialised as `failure` rather than collapsed into the diagnostics; the two
/// stay as distinct here as they are everywhere else (docs/error-model.md).
///
/// `scene` is non-null exactly when `failure` is empty. `diags` carries the
/// non-fatal observations either way, so callers should always render it.
struct SceneBuildResult {
    /// Const because the scene is shared, never owned exclusively: the viewer
    /// hands the same pointer to `SceneRenderer::beginUpload` while holding it
    /// as the resident scene, and every downstream consumer already spells it
    /// `shared_ptr<const render::Scene>`. Keeping the producer's handle mutable
    /// was the one gap in that chain.
    std::shared_ptr<const ir::render::Scene> scene;
    DiagnosticList diags;

    /// Set when the build could not produce a scene. Holds what would have been
    /// thrown, including everything observed before it.
    std::optional<Error> failure;
};

/// Outcome of `prepareSceneForTessellation`: a config and scene ready to feed a
/// `TessellationJob`, plus what prep observed. There is no `ok` — prep throws
/// when it cannot deliver, so a result that exists is a result that is ready.
struct ScenePrepResult {
    config::NHConfig config;
    ir::semantic::Scene scene;
    DiagnosticList diags;
};

/// Synchronous drive-to-completion shim for headless callers
/// (`nodehammer_bench`). Loads the config straight from the filesystem
/// (so `include = [...]` is resolved against the config's parent dir),
/// imports the geometry through the default `ImporterRegistry`, then
/// validates / selects / dedups / tessellates and returns the
/// `render::Scene`. Mirrors the `convert` CLI pipeline minus the export
/// stage and CLI surface.
SceneBuildResult buildSceneFromPaths(const std::filesystem::path &config_path,
                                     const std::filesystem::path &geometry_path);

/// Run validate + select + dedup against an already-parsed config and
/// already-imported semantic scene. Used by the viewer's BuildSession,
/// which has done the resolve + parse + import dance against bytes
/// pulled from a `ProjectFs`. No filesystem access. The result's
/// `scene` is handed off to `TessellationJob` for cooperative
/// iteration.
///
/// When `wedgeCut` is set, an azimuthal wedge cut is applied after dedup
/// (matching the `convert --angle-cut` pipeline ordering), so the scene
/// handed to tessellation already carries the Boolean-cut shapes.
ScenePrepResult prepareSceneForTessellationFromInputs(
    config::NHConfig config, ir::semantic::Scene scene,
    std::optional<tessellation::WedgeCutParams> wedgeCut = std::nullopt);

} // namespace nodehammer::pipeline

#pragma once

#include <config/config_ast.hpp>
#include <ir/diagnostics.hpp>
#include <ir/render.hpp>
#include <ir/semantic.hpp>
#include <tessellation/wedge_cut.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace nodehammer::pipeline {

/// Outcome of buildSceneFromPaths. On any pipeline-stage failure, scene is
/// null and diags carries the reason. Warnings can accompany a successful
/// build, so callers should always render diags regardless of success.
struct SceneBuildResult {
    /// Const because the scene is shared, never owned exclusively: the viewer
    /// hands the same pointer to `SceneRenderer::beginUpload` while holding it
    /// as the resident scene, and every downstream consumer already spells it
    /// `shared_ptr<const RenderScene>`. Keeping the producer's handle mutable
    /// was the one gap in that chain.
    std::shared_ptr<const ir::RenderScene> scene;
    ir::DiagnosticList diags;
};

/// Outcome of `prepareSceneForTessellation`. When `ok` is true, `config`
/// + `scene` are ready to be fed into a `TessellationJob`; when false,
/// `diags` describes why we stopped before reaching the tessellation
/// stage.
struct ScenePrepResult {
    config::NHConfig config;
    ir::SemanticScene scene;
    ir::DiagnosticList diags;
    bool ok{false};
};

/// Synchronous drive-to-completion shim for headless callers
/// (`nodehammer_bench`). Loads the config straight from the filesystem
/// (so `include = [...]` is resolved against the config's parent dir),
/// imports the geometry through the default `ImporterRegistry`, then
/// validates / selects / dedups / tessellates and returns the
/// `RenderScene`. Mirrors the `convert` CLI pipeline minus the export
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
    config::NHConfig config, ir::SemanticScene scene,
    std::optional<tessellation::WedgeCutParams> wedgeCut = std::nullopt);

} // namespace nodehammer::pipeline

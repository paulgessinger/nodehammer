#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <filesystem>
#include <memory>

namespace nodehammer {

/// Outcome of buildSceneFromPaths. On any pipeline-stage failure, scene is
/// null and diags carries the reason. Warnings can accompany a successful
/// build, so callers should always render diags regardless of success.
struct SceneBuildResult {
    std::shared_ptr<RenderScene> scene;
    DiagnosticList diags;
};

/// Outcome of `prepareSceneForTessellation`. When `ok` is true, `config`
/// + `scene` are ready to be fed into a `TessellationJob`; when false,
/// `diags` describes why we stopped before reaching the tessellation
/// stage.
struct ScenePrepResult {
    NHConfig config;
    SemanticScene scene;
    DiagnosticList diags;
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
ScenePrepResult prepareSceneForTessellationFromInputs(NHConfig config, SemanticScene scene);

} // namespace nodehammer

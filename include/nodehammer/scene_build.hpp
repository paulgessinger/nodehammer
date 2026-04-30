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

/// Synchronous drive-to-completion shim for headless callers (the
/// `convert` CLI subcommand and `nodehammer_bench`). Reads both files
/// into a `BagProjectFs`, runs the same `BuildSession` walk the viewer
/// uses, then tessellates the resulting scene synchronously and returns
/// the `RenderScene`. Geometry is FlatBuffer-only (`.nhb` / `.nhb.zst`);
/// the rest of the importer registry is reserved for the `convert`
/// subcommand.
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

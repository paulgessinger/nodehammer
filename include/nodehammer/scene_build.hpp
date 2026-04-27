#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace nodehammer {

/// Outcome of build_scene_from_paths. On any pipeline-stage failure, scene is
/// null and diags carries the reason. Warnings can accompany a successful
/// build, so callers should always render diags regardless of success.
struct SceneBuildResult {
    std::shared_ptr<RenderScene> scene;
    DiagnosticList diags;
};

/// Outcome of `prepare_scene_for_tessellation`. When `ok` is true, `config`
/// + `scene` are ready to be fed into a `TessellationJob`; when false,
/// `diags` describes why we stopped before reaching the tessellation
/// stage.
struct ScenePrepResult {
    NHConfig config;
    SemanticScene scene;
    DiagnosticList diags;
    bool ok{false};
};

/// Run the full disk-backed scene pipeline used by the viewer:
/// load+validate config, import the geometry, optional selection prune,
/// optional dedup, then tessellate to a RenderScene. Both paths must already
/// exist on the filesystem (real disk on native, MEMFS on the web build —
/// the AssetLoader is responsible for materialising files there before this
/// is called). `input_format` overrides extension-based format detection when
/// set.
SceneBuildResult build_scene_from_paths(const std::filesystem::path &config_path,
                                        const std::filesystem::path &input_path,
                                        std::optional<std::string> input_format = std::nullopt);

/// Run all stages *up to but not including* tessellation: config load,
/// validate, importer, selection prune, dedup. Returns a populated
/// `ScenePrepResult` whose `scene` can be handed off to `TessellationJob`
/// for cooperative iteration. Fast on `.nhb.zst`; slower on backends that
/// hit a heavy single-threaded library (ROOT/DD4hep/Geant4/GeoModel).
ScenePrepResult
prepare_scene_for_tessellation(const std::filesystem::path &config_path,
                               const std::filesystem::path &input_path,
                               std::optional<std::string> input_format = std::nullopt);

} // namespace nodehammer

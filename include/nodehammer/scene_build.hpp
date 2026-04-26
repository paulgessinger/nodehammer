#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render.hpp>

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

} // namespace nodehammer

#pragma once

// Public pipeline verbs. Each takes and returns handles, and reports through
// diagnostics rather than printing.

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/render.hpp>

#include <filesystem>

namespace nodehammer {

/// Outcome of `buildScene`. On failure `scene` is an invalid handle and `diags`
/// carries the reason. Warnings can accompany a successful build, so callers
/// should render `diags` either way.
struct BuildResult {
    RenderScene scene;
    DiagnosticList diags;
};

/// Run the conversion pipeline — import, validate, select, dedup, tessellate —
/// against a config file and a geometry file, and return the resulting render
/// scene.
[[nodiscard]] BuildResult buildScene(const std::filesystem::path &configPath,
                                     const std::filesystem::path &geometryPath);

} // namespace nodehammer

#pragma once

// Public pipeline verbs. Each takes and returns handles, and reports through
// diagnostics rather than printing.

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/render.hpp>
#include <nodehammer/scene.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

/// Outcome of `importGeometry`. Partial success is allowed: `scene` can be
/// valid and populated even when `diags` carries errors.
struct ImportResult {
    SemanticScene scene;
    DiagnosticList diags;
};

/// Import a geometry file into a semantic scene. The importer is resolved from
/// the file extension unless `format` names one explicitly.
[[nodiscard]] ImportResult importGeometry(const std::filesystem::path &path,
                                          std::string_view format = {});

/// Format names accepted by `importGeometry`, in registration order.
[[nodiscard]] std::vector<std::string> importFormats();

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

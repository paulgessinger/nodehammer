#pragma once

#include <config/config_ast.hpp>
#include <ir/render/exporter.hpp>

#include <filesystem>
#include <string_view>

namespace nodehammer {

/// Resolve the effective `ExportConfig` for one output path.
///
/// This is the single implementation of the precedence rules that decide what
/// a render export actually does:
///
///   1. the output format, from `formatHint` if given, else the extension;
///   2. that format's built-in defaults (`ExportConfig::defaultUnitScale` /
///      `defaultBakeUnitScale`);
///   3. the matching `[export.<fmt>]` table from the config, field by field —
///      each `std::optional` that is set overrides the default;
///   4. for GLB only, a fallback to `[export.gltf]` when there is no
///      `[export.glb]` table at all.
///
/// Rule 4 is deliberately table-level rather than per-field: a present but
/// partially-filled `[export.glb]` suppresses `[export.gltf]` entirely. That is
/// the behaviour `convert` has always had and it is preserved here verbatim;
/// no fixture exercises it, since none defines `[export.glb]`.
///
/// Lives at this layer, alongside `scene_build.hpp`, because it is the one
/// place that spans both sides — `NHConfig` in, `ExportConfig` out — and
/// neither `config/` nor `ir/render/` should have to know about the other.
///
/// A default-constructed `NHConfig` yields exactly the format defaults, so
/// callers with no config file need no separate entry point.
[[nodiscard]] ExportConfig resolveExportConfig(const NHConfig &cfg,
                                               const std::filesystem::path &outputPath,
                                               std::string_view formatHint = {});

} // namespace nodehammer

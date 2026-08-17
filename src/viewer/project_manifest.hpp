#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace nodehammer::viewer {

/// The archive-internal project manifest (`nodehammer.toml` at the archive root)
/// that makes a `.nhproj` self-describing: opening it "just builds" on any
/// platform without extension-based recognition. Only the `[project]` entry keys
/// are modelled today; `[view]` steer is applied by the steer cascade (R4).
struct ProjectManifest {
    std::string config_key;   ///< [project].config — the entry TOML config key
    std::string geometry_key; ///< [project].geometry — the entry geometry key
};

/// Parse a project's root `nodehammer.toml`. Returns the `[project]` entry keys
/// only when *both* config and geometry are present; returns `nullopt` on a parse
/// error or when the section/keys are absent, so the caller falls back to
/// extension-based recognition.
std::optional<ProjectManifest> parseProjectManifest(std::span<const std::byte> toml_bytes);

/// Serialize a `[project]` manifest to the TOML text that belongs at an archive's
/// root as `nodehammer.toml` — the inverse of `parseProjectManifest`. An archive
/// written with this opens with its roots already set instead of coming up blank
/// and waiting for the user to pick them out of the tree.
std::string serializeProjectManifest(const ProjectManifest &manifest);

} // namespace nodehammer::viewer

#pragma once

// One parsed configuration document, and the two slices the verbs actually
// take.
//
// Tier A only — the connector tier has no config surface at all, which is why
// the Lua front end needs no carve-out here (#41 §6).

#include <nodehammer/api.hpp>
#include <nodehammer/diagnostics.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

struct ConfigResult;

/// The half of a config that changes what the scene *is*: selection rules,
/// `hoist_orphans`, `deduplicate_shapes`, materials, `[[rules]]`, and the
/// tessellation defaults.
///
/// The seam is the question "does it change the scene?", and every field lands
/// cleanly on one side of it (#41 §3). A default-constructed slice means "no
/// config": no selection, no rules, dedup on, built-in tessellation defaults.
class SceneConfig {
  public:
    /// True when this slice came from a real document. A default-constructed
    /// slice is usable — it means the built-in defaults — so this reports
    /// provenance, not usability.
    [[nodiscard]] NH_API bool valid() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    friend struct api::Access;
};

/// The half that changes only how a final scene is *serialized*: the
/// `[export.*]` tables — unit scale, bake, `multi_scene`, the scene-name
/// separator. Nothing here can alter geometry, which is why `RenderScene::write`
/// takes this and not a whole `Config`.
class OutputConfig {
  public:
    /// True when this slice came from a real document. A default-constructed
    /// slice resolves to each format's built-in defaults.
    [[nodiscard]] NH_API bool valid() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    friend struct api::Access;
};

/// A parsed TOML (or Lua) configuration.
///
/// Opaque because the underlying AST is the most volatile struct in the
/// project; publishing it by value would ABI-freeze it and drag its whole
/// transitive type set — and nlohmann — into this interface (#41 §7).
///
/// Both entry points are thin wrappers over the internal loader: extension
/// dispatch, validation, and the `scene()` / `output()` slicing, and no
/// resolution logic of their own (#41 §11).
class Config {
  public:
    /// Read a config file. `.lua` dispatches to the scripting front end (only
    /// in a build that has it — otherwise a diagnostic, since the format is not
    /// known until the extension is looked at); anything else is TOML.
    ///
    /// `include = [...]` resolves against the file's own directory, and nested
    /// includes against theirs.
    [[nodiscard]] NH_API static ConfigResult read(const std::filesystem::path &path);

    /// Parse TOML held in memory. `include = [...]` resolves against `baseDir`.
    ///
    /// An empty `baseDir` means the content has no location, so its includes
    /// resolve against nothing and are reported as not found. It does **not**
    /// mean the process's working directory: deciding where "here" is belongs
    /// to the application, and a caller that wants the working directory says
    /// so by passing it (#41 §11, amended after step 5b).
    [[nodiscard]] NH_API static ConfigResult parse(std::string_view toml,
                                                   const std::filesystem::path &baseDir = {});

    /// Config formats this build understands: "toml", plus "lua" where the
    /// scripting front end is compiled in.
    [[nodiscard]] NH_API static std::vector<std::string> formats();

    /// The scene-affecting slice. Shares the parsed document with this handle
    /// rather than copying it.
    [[nodiscard]] NH_API SceneConfig scene() const;

    /// The serialization-affecting slice.
    [[nodiscard]] NH_API OutputConfig output() const;

    /// True when this handle refers to a parsed document.
    [[nodiscard]] NH_API bool valid() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    friend struct api::Access;
};

struct ConfigResult {
    Config config;
    DiagnosticList diags;
};

} // namespace nodehammer

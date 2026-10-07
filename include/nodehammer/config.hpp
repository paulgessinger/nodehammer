#pragma once

// One parsed configuration document, and the two slices the verbs actually
// take.
//
// Configuration and its loaders belong to the full processing library.

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/visibility.hpp>

#include <filesystem>
#include <memory>
#include <span>
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

    /// Opaque state — see `DiagnosticList::Impl`.
    struct Impl;

    SceneConfig() noexcept = default;

    /// Adopt state the library built.
    explicit SceneConfig(std::shared_ptr<const Impl> impl) noexcept;

    /// The document behind a slice that has one. Throws `Error` when `valid()`
    /// is false.
    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
};

/// The half that changes only how a final scene is *serialized*: the
/// `[export.*]` tables — unit scale, bake, `multi_scene`, the scene-name
/// separator. Nothing here can alter geometry, which is why `write`
/// takes this and not a whole `Config`.
class OutputConfig {
  public:
    /// True when this slice came from a real document. A default-constructed
    /// slice resolves to each format's built-in defaults.
    [[nodiscard]] NH_API bool valid() const noexcept;

    /// Opaque state — see `SceneConfig::impl`.
    struct Impl;

    OutputConfig() noexcept = default;

    /// Adopt state the library built.
    explicit OutputConfig(std::shared_ptr<const Impl> impl) noexcept;

    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
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
    /// The scene-affecting slice. Shares the parsed document with this handle
    /// rather than copying it.
    [[nodiscard]] NH_API SceneConfig scene() const;

    /// The serialization-affecting slice.
    [[nodiscard]] NH_API OutputConfig output() const;

    /// True when this handle refers to a parsed document.
    [[nodiscard]] NH_API bool valid() const noexcept;

    /// Opaque state — see `DiagnosticList::Impl`.
    struct Impl;

    Config() noexcept = default;

    /// Adopt state the library built.
    explicit Config(std::shared_ptr<const Impl> impl) noexcept;

    /// The document behind a live handle. Throws `Error` when `valid()` is
    /// false.
    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
};

struct ConfigResult {
    Config config;
    DiagnosticList diags;
};

/// Read TOML or Lua from a file; includes resolve relative to that file.
[[nodiscard]] NH_API ConfigResult readConfig(const std::filesystem::path &path);
/// Parse TOML. Empty baseDir means no location, not the working directory.
[[nodiscard]] NH_API ConfigResult fromToml(std::string_view toml,
                                           const std::filesystem::path &baseDir = {});
/// Return validation observations; throws if the file cannot be read.
[[nodiscard]] NH_API DiagnosticList checkConfig(const std::filesystem::path &path);
[[nodiscard]] NH_API DiagnosticList checkConfigString(std::string_view toml,
                                                      const std::filesystem::path &baseDir = {});
[[nodiscard]] NH_API std::span<const std::string_view> configFormats();

} // namespace nodehammer

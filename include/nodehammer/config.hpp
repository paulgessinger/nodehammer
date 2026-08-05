#pragma once

// One parsed configuration document, and the two slices the verbs actually
// take.
//
// Tier A only — the connector tier has no config surface at all, which is why
// the Lua front end needs no carve-out here (#41 §6).

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
/// separator. Nothing here can alter geometry, which is why `RenderScene::write`
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
    /// Read a config file. `.lua` dispatches to the scripting front end;
    /// anything else is TOML. Both front ends are in every build, so this is a
    /// choice of parser rather than a capability question.
    ///
    /// `include = [...]` resolves against the file's own directory, and nested
    /// includes against theirs.
    ///
    /// Throws `Error` if the document does not load or does not validate: unlike
    /// a scene there is no half-built config worth returning, so the loader's
    /// errors become the exception and only its warnings ride back in `diags`.
    [[nodiscard]] NH_API static ConfigResult read(const std::filesystem::path &path);

    /// Parse TOML held in memory. `include = [...]` resolves against `baseDir`.
    ///
    /// An empty `baseDir` means the content has no location, so its includes
    /// resolve against nothing — and an unresolvable include is a load error,
    /// so this throws. It does **not** mean the process's working directory:
    /// deciding where "here" is belongs to the application, and a caller that
    /// wants the working directory says so by passing it (#41 §11, amended
    /// after step 5b).
    [[nodiscard]] NH_API static ConfigResult parse(std::string_view toml,
                                                   const std::filesystem::path &baseDir = {});

    /// Report on a document instead of loading it.
    ///
    /// `read` promises a config, so a document that will not parse is fatal to
    /// it. This promises a *report*, so the same document is simply its answer:
    /// the returned list carries every problem the loader and the validator
    /// found, at every severity, and an empty list means the document is sound.
    /// It is the same work behind both — one collector, two promises
    /// (docs/error-model.md).
    ///
    /// Still throws when there is no document to report on: a file that will not
    /// open says nothing about a document's contents, which is what this
    /// promised to describe.
    ///
    /// This is what `nodehammer validate-config` does, available to a caller
    /// that wants to check a config without committing to using it — an editor,
    /// a CI step, a settings dialog.
    [[nodiscard]] NH_API static DiagnosticList check(const std::filesystem::path &path);

    /// The in-memory counterpart, as `parse` is to `read`.
    ///
    /// Named rather than overloaded because a string literal converts to both
    /// `path` and `string_view`, so `check("cfg.toml")` would be ambiguous —
    /// and a caller who guessed wrong would be checking a *filename* as though
    /// it were a document. `read`/`parse` avoid this by being two verbs; this
    /// pair cannot, so the text face says so in its name.
    [[nodiscard]] NH_API static DiagnosticList
    checkString(std::string_view toml, const std::filesystem::path &baseDir = {});

    /// Config formats this build understands: "toml" and "lua". Constant, unlike
    /// `SemanticScene::formats` — both front ends are compiled in
    /// unconditionally, so it is a build-independent answer given by the same
    /// query every type answers. A view over library-lifetime storage.
    [[nodiscard]] NH_API static std::span<const std::string_view> formats();

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

} // namespace nodehammer

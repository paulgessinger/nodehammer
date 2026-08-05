#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::config {

/// A config and what was observed producing it.
///
/// What `diags` may contain depends on which face produced it: the `load…`
/// faces promise a config and throw rather than return a broken one, so theirs
/// carries warnings only. The `collect…` faces promise a *report*, so theirs
/// carries everything, and `config` is whatever could be salvaged
/// (docs/error-model.md).
struct ConfigResult {
    NHConfig config;
    DiagnosticList diags;
};

/// Synchronous byte fetcher used by `parseAndMerge` to look up the bytes
/// for a logical key (e.g. an include's resolved key). Returns nullopt
/// when the key is unknown — `parseAndMerge` surfaces this as a
/// "include not found" diagnostic. Callers (typically the BuildSession)
/// guarantee that all referenced keys are already resolved before
/// invoking `parseAndMerge`, so a nullopt mid-walk indicates a bug or
/// a stale include reference.
using IncludeFetcher =
    std::function<std::optional<std::span<const std::byte>>(std::string_view absolute_key)>;

struct ConfigLoader {
    // ── Faces that promise a config ──────────────────────────────────────────
    //
    // Each throws `Error` if the document does not load, carrying every
    // problem the collector found rather than only the first. The returned
    // `diags` is warnings.

    /// Load a config file and resolve its include tree against the file's own
    /// directory.
    [[nodiscard]] static ConfigResult loadFromFile(const std::filesystem::path &path);

    /// Parse `content` as TOML and resolve its include tree against `baseDir`
    /// on the filesystem — this is `loadFromFile` minus the initial read, and
    /// runs through the same fetcher and the same merge walk.
    ///
    /// `baseDir` is the caller's to choose and the loader never invents one: an
    /// empty `baseDir` means the content has no location, so its includes
    /// resolve against nothing and are reported as not found. A caller that
    /// wants them read relative to the process's working directory says so by
    /// passing it — the choice belongs where the program's notion of "here"
    /// does, which for the Lua front end is cmd_config_lua.cpp.
    [[nodiscard]] static ConfigResult loadFromString(std::string_view content,
                                                     std::string_view sourceName = "<string>",
                                                     const std::filesystem::path &baseDir = {});

    /// Parse `root_bytes` as TOML, recursively resolve `include = "..."`
    /// directives by calling `fetcher` for each computed include key, merge
    /// everything, and run the config-AST parser. Cycle detection is by
    /// string-key equality in a visited set.
    [[nodiscard]] static ConfigResult parseAndMerge(std::span<const std::byte> root_bytes,
                                                    std::string_view root_key,
                                                    IncludeFetcher fetcher);

    // ── Faces that promise a report ──────────────────────────────────────────
    //
    // The loader collects rather than stops, because naming every problem in a
    // document is the whole job — so these hand the collection back, errors and
    // all, and never throw for anything the document *says*. They still throw
    // when there is no document to report on: a file that will not open.
    //
    // This is what `nodehammer validate-config` and `Config::check` are built
    // from, and why the doctrine's "no result type represents failure" does not
    // apply to them: the report *is* the result.

    [[nodiscard]] static ConfigResult collectFromFile(const std::filesystem::path &path);

    [[nodiscard]] static ConfigResult collectFromString(std::string_view content,
                                                        std::string_view sourceName = "<string>",
                                                        const std::filesystem::path &baseDir = {});

    [[nodiscard]] static ConfigResult collectAndMerge(std::span<const std::byte> root_bytes,
                                                      std::string_view root_key,
                                                      IncludeFetcher fetcher);

    /// The on-disk fetcher: keys are filesystem paths, read and cached so the
    /// spans stay valid for the whole walk. Named here rather than kept private
    /// because the Lua front end resolves its `include()` / `use()` through the
    /// same seam, and a second implementation of "read the file named by this
    /// key" is exactly the drift this class exists to avoid.
    [[nodiscard]] static IncludeFetcher filesystemFetcher();

    /// The fetcher for a load with no location: it resolves nothing, so an
    /// include is reported as not found rather than looked up somewhere the
    /// caller never named — in particular, not against the process's working
    /// directory. Deciding that the working directory is the right base is an
    /// application-level choice, so it is made by a caller passing that
    /// directory in, never here.
    [[nodiscard]] static IncludeFetcher unrootedFetcher();

    /// Read only the top-level `include = [...]` array of a TOML buffer —
    /// does NOT recurse. Used by the BuildSession's include-graph walk
    /// to discover the next round of keys to resolve. Returns the raw,
    /// unresolved relative paths in declared order; returns empty on
    /// parse failure or when no `include` key is present.
    [[nodiscard]] static std::vector<std::string>
    peekIncludesFromBytes(std::span<const std::byte> bytes);

    /// Compute the absolute logical key of an include, given the parent
    /// file's absolute key and the relative path from `include = "..."`.
    /// Mirrors the filesystem `parent_dir / rel` semantic but operates on
    /// strings, so it works for any project keying scheme (URL paths,
    /// archive paths, basename bag keys). Result is generic-form
    /// (forward-slash) and lexically normalised.
    [[nodiscard]] static std::string resolveIncludeKey(std::string_view parent_key,
                                                       std::string_view rel);
};

} // namespace nodehammer::config

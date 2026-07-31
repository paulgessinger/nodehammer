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

/// Returned by ConfigLoader. Always valid — check diags.hasErrors() to determine whether
/// the config can be used. On a fatal parse error, config is default-initialized.
struct ConfigResult {
    NHConfig config;
    diagnostics::DiagnosticList diags;
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

    /// Parse `root_bytes` as TOML, recursively resolve `include = "..."`
    /// directives by calling `fetcher` for each computed include key,
    /// merge everything, and run the config-AST parser. Cycle detection
    /// is by string-key equality in a visited set. Diagnostics report
    /// missing/unresolvable includes; the caller (BuildSession) is
    /// responsible for ensuring the fetcher can satisfy every reachable
    /// key before invocation.
    [[nodiscard]] static ConfigResult parseAndMerge(std::span<const std::byte> root_bytes,
                                                    std::string_view root_key,
                                                    IncludeFetcher fetcher);
};

} // namespace nodehammer::config

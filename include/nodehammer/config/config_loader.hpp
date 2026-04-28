#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

/// Returned by ConfigLoader. Always valid — check diags.hasErrors() to determine whether
/// the config can be used. On a fatal parse error, config is default-initialized.
struct ConfigResult {
    NHConfig config;
    DiagnosticList diags;
};

struct ConfigLoader {
    [[nodiscard]] static ConfigResult loadFromFile(const std::filesystem::path &path);
    [[nodiscard]] static ConfigResult loadFromString(std::string_view content,
                                                     std::string_view sourceName = "<string>");

    /// Read only the top-level `include = [...]` array of a single TOML file —
    /// does NOT recurse into the included files. The web viewer's AssetLoader
    /// drives recursion by calling this once per file as each one lands in
    /// MEMFS, diffing against an already-fetched set, and enqueueing the new
    /// URLs. Returns the raw, unresolved relative paths in their declared
    /// order; returns an empty vector on parse failure or when no `include`
    /// key is present.
    [[nodiscard]] static std::vector<std::string> peekIncludes(const std::filesystem::path &path);
};

} // namespace nodehammer

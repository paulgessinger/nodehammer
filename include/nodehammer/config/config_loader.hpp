#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>

#include <filesystem>
#include <string_view>

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
};

} // namespace nodehammer

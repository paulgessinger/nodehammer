#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>

#include <expected>
#include <filesystem>
#include <string_view>

namespace nodehammer {

struct ConfigLoader {
    /// Parse a TOML file from disk. Returns the config or a non-empty DiagnosticList on failure.
    [[nodiscard]] static std::expected<NHConfig, DiagnosticList>
    loadFromFile(const std::filesystem::path &path);

    /// Parse TOML from a string (useful for tests). sourceName appears in diagnostics.
    [[nodiscard]] static std::expected<NHConfig, DiagnosticList>
    loadFromString(std::string_view content, std::string_view sourceName = "<string>");
};

} // namespace nodehammer

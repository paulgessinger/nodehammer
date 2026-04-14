#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

/// Result of a single import call. Partial success is allowed:
/// scene may be populated even when diags.hasErrors() is true.
struct ImportResult {
    SemanticScene scene;
    DiagnosticList diags;
};

/// Pure interface for all geometry importers.
class ISemanticImporter {
  public:
    virtual ~ISemanticImporter() = default;

    /// Human-readable format identifier, e.g. "synthetic", "gdml", "tgeo".
    [[nodiscard]] virtual std::string_view formatName() const noexcept = 0;

    /// File extensions this importer claims, without leading dot, e.g. {"gdml"}.
    /// Returns empty vector for format-name-only importers (e.g. synthetic).
    [[nodiscard]] virtual std::vector<std::string> supportedExtensions() const = 0;

    /// Perform the import. For importers that do not use a file path (e.g. synthetic),
    /// the path argument is ignored.
    [[nodiscard]] virtual ImportResult import(const std::filesystem::path &path) const = 0;
};

} // namespace nodehammer

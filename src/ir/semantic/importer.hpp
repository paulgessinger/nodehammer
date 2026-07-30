#pragma once

#include <ir/diagnostics.hpp>
#include <ir/semantic.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::ir {

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

/// Owns a collection of ISemanticImporter instances. Lookup is by format name or
/// file extension. Not a singleton — construct one per pipeline invocation
/// or share a long-lived instance.
class ImporterRegistry {
  public:
    /// Register an importer. The registry takes ownership.
    void registerImporter(std::unique_ptr<ISemanticImporter> importer);

    /// Look up by exact format name (case-sensitive). Returns nullptr if not found.
    [[nodiscard]] const ISemanticImporter *findByFormat(std::string_view formatName) const noexcept;

    /// Look up by file extension (without leading dot, case-insensitive).
    /// Returns nullptr if no importer claims that extension.
    [[nodiscard]] const ISemanticImporter *findByExtension(std::string_view ext) const noexcept;

    /// Resolve an importer from a path and an optional explicit format name.
    /// If formatName is non-empty it takes precedence; otherwise the path
    /// extension is used. Returns nullptr if resolution fails.
    [[nodiscard]] const ISemanticImporter *resolve(const std::filesystem::path &path,
                                                   std::string_view formatName = {}) const noexcept;

    /// All registered importers, in registration order.
    [[nodiscard]] const std::vector<std::unique_ptr<ISemanticImporter>> &importers() const noexcept;

    /// Build a registry pre-populated with all built-in importers.
    /// Currently registers: SyntheticImporter.
    [[nodiscard]] static ImporterRegistry makeDefault();

  private:
    std::vector<std::unique_ptr<ISemanticImporter>> importers_;
};

} // namespace nodehammer::ir

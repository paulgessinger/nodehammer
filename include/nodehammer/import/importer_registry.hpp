#pragma once

#include <nodehammer/import/importer.hpp>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace nodehammer {

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

} // namespace nodehammer

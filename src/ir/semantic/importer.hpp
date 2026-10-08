#pragma once

#include <diagnostics.hpp>
#include <ir/semantic.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <nodehammer/import_options.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::ir {

/// Result of a single import call. The scene is always valid: an importer that could not read its
/// input throws `Error` rather than returning an empty scene beside an explanation, so `diags`
/// carries only what was observed about a scene that exists — an unknown shape, a missing material
/// (docs/error-model.md).
struct ImportResult {
    semantic::Scene scene;
    DiagnosticList diags;
};

/// Backend-owned schema shared by API validation and the optional CLI adapter.
struct ImporterOptionSpec {
    std::string_view name;
    ImporterOptionValue defaultValue;
    /// Empty for options exposed only programmatically. No CLI11 dependency.
    std::string_view cliFlag{};
    std::string_view description{};
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

    [[nodiscard]] virtual std::span<const ImporterOptionSpec> optionSpecs() const { return {}; }
    /// Receives this backend's validated values, including defaults.
    virtual void configure(const ImporterOptions &) {}

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
    /// Throws when explicitly configured backend options target another format.
    [[nodiscard]] const ISemanticImporter *resolve(const std::filesystem::path &path,
                                                   std::string_view formatName = {}) const;

    /// All registered importers, in registration order.
    [[nodiscard]] const std::vector<std::unique_ptr<ISemanticImporter>> &importers() const noexcept;

    /// Build a registry pre-populated with all built-in importers.
    [[nodiscard]] static ImporterRegistry makeDefault(const SemanticReadOptions &options = {});
    /// Validate the complete option set before configuring any importer.
    void configure(const ImporterOptions &options);
    /// Set one option while retaining other explicit values (used by CLI adapters).
    void setOption(std::string name, ImporterOptionValue value);
    using Decorator =
        std::function<std::unique_ptr<ISemanticImporter>(std::unique_ptr<ISemanticImporter>)>;
    void decorate(std::string_view format, const Decorator &decorator);

  private:
    [[nodiscard]] const ISemanticImporter *resolveUnchecked(const std::filesystem::path &path,
                                                            std::string_view formatName) const;
    ImporterOptions options_{};
    std::vector<std::unique_ptr<ISemanticImporter>> importers_;
};

} // namespace nodehammer::ir

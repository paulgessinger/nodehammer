#pragma once

#include <diagnostics.hpp>
#include <ir/semantic.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace nodehammer::ir {

struct SemanticExportConfig {};

struct SemanticExportResult {
    diagnostics::DiagnosticList diags;
};

/// Pure interface for semantic scene exporters (JSON/NHB/...).
class ISemanticExporter {
  public:
    virtual ~ISemanticExporter() = default;

    /// Human-readable format identifier, e.g. "json", "nhb".
    [[nodiscard]] virtual std::string_view formatName() const noexcept = 0;

    /// File extensions claimed by this exporter, without leading dot.
    /// Compound extensions are allowed (e.g. "json.zst", "nhb.zst").
    [[nodiscard]] virtual std::vector<std::string> supportedExtensions() const = 0;

    /// Write semantic scene to path.
    [[nodiscard]] virtual SemanticExportResult write(const semantic::Scene &scene,
                                                     const std::filesystem::path &path,
                                                     const SemanticExportConfig &config) const = 0;
};

class SemanticExporterRegistry {
  public:
    void registerExporter(std::unique_ptr<ISemanticExporter> exporter);

    [[nodiscard]] const ISemanticExporter *findByFormat(std::string_view formatName) const noexcept;
    [[nodiscard]] const ISemanticExporter *findByExtension(std::string_view ext) const noexcept;
    [[nodiscard]] const ISemanticExporter *resolve(const std::filesystem::path &path,
                                                   std::string_view formatHint = {}) const noexcept;

    [[nodiscard]] std::span<const std::unique_ptr<ISemanticExporter>> exporters() const noexcept;

    [[nodiscard]] static SemanticExporterRegistry makeDefault();

  private:
    std::vector<std::unique_ptr<ISemanticExporter>> exporters_;
};

} // namespace nodehammer::ir

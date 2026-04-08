#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

// ── Export configuration ──────────────────────────────────────────────────────

/// glTF/GLB-specific export options.
struct GltfExportOptions {
    bool multiScene{false};                ///< Split render tree into multiple glTF scenes
    std::string sceneNameSeparator{" > "}; ///< Separator for hierarchical scene names
};

struct ExportConfig {
    enum class Format { GLB, GLTF, OBJ } format{Format::GLB};
    double unitScale{1.0};     ///< Scale factor applied to the root node (e.g. 0.01 for cm→m)
    bool bakeUnitScale{false}; ///< If true, scale vertices & translations instead of root matrix

    /// Format-specific options (only the matching format's options are used).
    GltfExportOptions gltf;

    /// Default unit scale for each format when the source system uses centimetres (DD4hep/TGeo).
    /// glTF/GLB: 0.01  (spec mandates metres)
    /// OBJ:      0.01 (unitless; treat source cm as mm for typical DCC tool expectations)
    static constexpr double defaultUnitScale(Format fmt) noexcept {
        switch (fmt) {
        case Format::GLB:
        case Format::GLTF:
            return 0.01;
        case Format::OBJ:
            return 0.01;
        }
        return 1.0;
    }
};

// ── ExportResult ──────────────────────────────────────────────────────────────

struct ExportResult {
    DiagnosticList diags;
};

// ── IExporter ─────────────────────────────────────────────────────────────────

class IExporter {
  public:
    virtual ~IExporter() = default;

    /// Human-readable format identifier, e.g. "gltf", "obj".
    [[nodiscard]] virtual std::string_view formatName() const noexcept = 0;

    /// File extensions this exporter claims, with leading dot, e.g. {".glb", ".gltf"}.
    [[nodiscard]] virtual std::vector<std::string> supportedExtensions() const = 0;

    /// Write the scene to path. Format (GLB vs GLTF vs OBJ) may be inferred from
    /// config.format or from the path extension.
    [[nodiscard]] virtual ExportResult write(const RenderScene &scene,
                                             const std::filesystem::path &path,
                                             const ExportConfig &config) const = 0;
};

// ── ExporterRegistry ──────────────────────────────────────────────────────────

class ExporterRegistry {
  public:
    void registerExporter(std::unique_ptr<IExporter> exporter);

    /// Look up by exact format name (case-sensitive).
    [[nodiscard]] const IExporter *findByFormat(std::string_view formatName) const noexcept;

    /// Look up by file extension (with leading dot, case-insensitive).
    [[nodiscard]] const IExporter *findByExtension(std::string_view ext) const noexcept;

    /// Resolve an exporter from a path and an optional explicit format name.
    /// formatHint takes precedence when non-empty; otherwise the path extension is used.
    [[nodiscard]] const IExporter *resolve(const std::filesystem::path &path,
                                           std::string_view formatHint = {}) const noexcept;

  private:
    std::vector<std::unique_ptr<IExporter>> exporters_;
};

/// Build a registry pre-populated with GltfExporter and ObjExporter.
[[nodiscard]] ExporterRegistry makeDefaultExporterRegistry();

} // namespace nodehammer

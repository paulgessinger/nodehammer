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

struct ExportConfig {
    enum class Format { GLB, GLTF, OBJ } format{Format::GLB};
    bool embedExtras{false}; ///< Embed provenance info in glTF extras
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

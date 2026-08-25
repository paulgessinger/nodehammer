#pragma once

#include <ir/render/exporter.hpp>

namespace nodehammer::ir {

/// Writes a render::Scene as `.nhr` — the render IR's own FlatBuffer form.
///
/// In the registry rather than special-cased above it. `RenderScene::write` used
/// to dispatch `.nhr` itself, ahead of the registry that would have called it
/// unknown, which left the two front doors disagreeing about what exists:
/// `RenderScene::formats()` reported `nhr` and `convert -o x.nhr` failed. One
/// exporter answers both.
///
/// A `.zst` suffix compresses, the same convention the semantic side uses and
/// the same one `read` applies in reverse.
class RenderFlatbufferExporter final : public IRenderExporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    void write(const render::Scene &scene, const std::filesystem::path &path,
               const ExportConfig &config) const override;
};

} // namespace nodehammer::ir

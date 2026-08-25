#pragma once

#include <ir/render/exporter.hpp>

namespace nodehammer::ir {

/// Writes a render::Scene as JSON — the render IR made readable.
///
/// `dump-render` used to do this inline, with a `nlohmann::json j = scene` and a
/// hand-rolled write, which is why `convert` could not produce one and why the
/// command existed at all.
///
/// Its format name is `render-json`, not `json`, because the *semantic* scene
/// has a JSON form too and they are different documents. Both claim `.json`, so
/// a caller who means this one says so: `convert` consults the semantic registry
/// first, which makes the bare extension mean the shallower of the two.
class RenderJsonExporter final : public IRenderExporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    void write(const render::Scene &scene, const std::filesystem::path &path,
               const ExportConfig &config) const override;
};

} // namespace nodehammer::ir

#pragma once

#include <ir/semantic/exporter.hpp>

namespace nodehammer {

class SemanticJsonExporter final : public ISemanticExporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] SemanticExportResult write(const SemanticScene &scene,
                                             const std::filesystem::path &path,
                                             const SemanticExportConfig &config) const override;
};

} // namespace nodehammer

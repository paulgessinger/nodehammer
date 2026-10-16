#pragma once
#include <ir/semantic/importer.hpp>

namespace nodehammer::ir {
class GdmlImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override { return "gdml"; }
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override { return {"gdml"}; }
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;
};
} // namespace nodehammer::ir

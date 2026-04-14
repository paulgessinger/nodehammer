#pragma once

#include <nodehammer/import/importer.hpp>

#include <string_view>
#include <vector>

namespace nodehammer {

/// ISemanticImporter that reads a FlatBuffer-encoded SemanticScene (.nhb/.nhb.zst).
/// Format name: "flatbuffer"   Extensions: "nhb", "nhb.zst"
class FlatBufferImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;
};

} // namespace nodehammer

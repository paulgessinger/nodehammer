#pragma once

#include <nodehammer/import/importer.hpp>

#include <string_view>
#include <vector>

namespace nodehammer {

/// IImporter that reads a FlatBuffer-encoded SemanticScene (.nhb).
/// Format name: "flatbuffer"   Extension: "nhb"
class FlatBufferImporter final : public IImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;
};

} // namespace nodehammer

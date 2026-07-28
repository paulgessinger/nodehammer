#pragma once

#include <nodehammer/ir/semantic/importer.hpp>

#include <string_view>
#include <vector>

namespace nodehammer {

/// ISemanticImporter that reads a previously-dumped SemanticScene JSON file.
/// Format name: "json"   Extension: ".json"
class JsonImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] detail::ImportResult import(const std::filesystem::path &path) const override;
};

} // namespace nodehammer

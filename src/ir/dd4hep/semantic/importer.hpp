#pragma once

#include <ir/semantic/importer.hpp>

#include <string_view>
#include <vector>

namespace dd4hep {
class Detector;
}

namespace nodehammer::ir {

/// ISemanticImporter for DD4hep compact XML geometry descriptions.
/// Format name: "dd4hep"   Extensions: none — .xml is ambiguous, explicit --input-format required.
class DD4hepImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;

    /// Traverse an already-constructed `Detector`. Never loads a compact file.
    [[nodiscard]] ImportResult import(dd4hep::Detector &detector) const;
};

} // namespace nodehammer::ir

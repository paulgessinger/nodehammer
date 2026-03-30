#pragma once

#include <nodehammer/import/importer.hpp>

#include <string_view>
#include <vector>

class TGeoManager;

namespace nodehammer {

/// IImporter for plain ROOT files containing a TGeoManager.
/// Format name: "tgeo"   Extension: ".root"
class TGeoImporter final : public IImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;

    /// Load a .root file. Safe to manipulate gGeoManager; caller accepts that.
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;

    /// Traverse an already-constructed manager. Never touches gGeoManager.
    [[nodiscard]] ImportResult import(TGeoManager *mgr) const;
};

} // namespace nodehammer

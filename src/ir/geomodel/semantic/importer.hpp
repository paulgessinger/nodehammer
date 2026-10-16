#pragma once
#include <ir/semantic/importer.hpp>
class GeoVPhysVol;
namespace nodehammer::ir {
// Nominal geometry only. The returned scene owns no GeoModel pointers.
ImportResult importGeoModel(const GeoVPhysVol &root, std::string source = "<geomodel>");
class GeoModelImporter final : public ISemanticImporter {
  public:
    std::string_view formatName() const noexcept override { return "geomodel"; }
    std::vector<std::string> supportedExtensions() const override {
        return {"db", "sqlite", "sqlite3"};
    }
    std::span<const ImporterOptionSpec> optionSpecs() const override;
    void configure(const ImporterOptions &options) override;
    ImportResult import(const std::filesystem::path &path) const override;

  private:
    bool allowOpaqueShapes_ = false;
};
} // namespace nodehammer::ir

// GeoModelRead writes progress to stdout even at log level zero. Keep native
// CLI JSON output clean without changing process descriptors in SDK callers.
#include "import_log_guard.hpp"
#include "run_internal.hpp"
#include <diagnostic_codes.hpp>
#include <ir/semantic/importer.hpp>
namespace nodehammer::cli::detail {
namespace {
class NativeGeoModelImporter final : public ir::ISemanticImporter {
  public:
    explicit NativeGeoModelImporter(std::unique_ptr<ir::ISemanticImporter> importer)
        : importer_(std::move(importer)) {}
    std::span<const ir::ImporterOptionSpec> optionSpecs() const override {
        return importer_->optionSpecs();
    }
    void configure(const ImporterOptions &options) override { importer_->configure(options); }
    std::string_view formatName() const noexcept override { return importer_->formatName(); }
    std::vector<std::string> supportedExtensions() const override {
        return importer_->supportedExtensions();
    }
    ir::ImportResult import(const std::filesystem::path &path) const override {
        const ImportLogGuard guard{codes::kFatalGeoModelRead,
                                   "cannot redirect GeoModel reader output"};
        return importer_->import(path);
    }

  private:
    std::unique_ptr<ir::ISemanticImporter> importer_;
};
} // namespace
void registerCmdGeoModelNative(CLI::App &, const CliOptions &, ir::ImporterRegistry &registry) {
    registry.decorate("geomodel", [](std::unique_ptr<ir::ISemanticImporter> importer) {
        return std::make_unique<NativeGeoModelImporter>(std::move(importer));
    });
}
} // namespace nodehammer::cli::detail

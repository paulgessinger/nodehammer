// GeoModelRead writes progress to stdout even at log level zero. Keep native
// CLI JSON output clean without changing process descriptors in SDK callers.
#include "run_internal.hpp"
#include <cstdio>
#include <diagnostic_codes.hpp>
#include <iostream>
#include <ir/semantic/importer.hpp>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
namespace nodehammer::cli::detail {
namespace {
class ImportLogGuard {
  public:
    ImportLogGuard() {
        std::cout.flush();
        std::fflush(stdout);
#ifdef _WIN32
        saved_ = _dup(_fileno(stdout));
        const bool ok = saved_ >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) >= 0;
#else
        saved_ = dup(fileno(stdout));
        const bool ok = saved_ >= 0 && dup2(fileno(stderr), fileno(stdout)) >= 0;
#endif
        if (!ok) {
            closeSaved();
            throw Error{codes::kFatalGeoModelRead, "cannot redirect GeoModel reader output"};
        }
    }
    ~ImportLogGuard() {
        std::cout.flush();
        std::fflush(stdout);
#ifdef _WIN32
        _dup2(saved_, _fileno(stdout));
#else
        dup2(saved_, fileno(stdout));
#endif
        closeSaved();
    }
    ImportLogGuard(const ImportLogGuard &) = delete;
    ImportLogGuard &operator=(const ImportLogGuard &) = delete;

  private:
    void closeSaved() {
        if (saved_ < 0)
            return;
#ifdef _WIN32
        _close(saved_);
#else
        close(saved_);
#endif
    }
    int saved_ = -1;
};
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
        const ImportLogGuard guard;
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

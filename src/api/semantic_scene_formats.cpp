#include <api/handles_semantic.hpp>
#include <nodehammer/io.hpp>

#include <diagnostic_codes.hpp>
#include <ir/semantic/exporter.hpp>
#include <ir/semantic/importer.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

namespace nodehammer {
namespace {

void appendUnique(std::vector<std::string> &out, std::string_view name) {
    if (std::find(out.begin(), out.end(), name) == out.end()) {
        out.emplace_back(name);
    }
}

} // namespace

SemanticResult readSemantic(const std::filesystem::path &path, const SemanticReadOptions &options) {
    const auto registry = ir::ImporterRegistry::makeDefault();
    const auto *importer = registry.resolve(path, options.format);
    if (importer == nullptr) {
        // The string-dispatched entry point necessarily fails here rather than
        // at link time: "dd4hep" is a value, not a type, so nothing earlier
        // could have known this build lacks the backend (#41 §5).
        throw Error{
            codes::kFatalImportFormatUnknown,
            options.format.empty()
                ? std::format("no importer claims the extension of '{}'", path.string())
                : std::format("no importer named '{}' in this build; see semanticReadFormats()",
                              options.format),
            path.string()};
    }
    try {
        return api::asHandle(importer->import(path));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, path.string());
    }
}

std::span<const std::string_view> semanticReadFormats() {
    // Built once and kept: which formats a build has is fixed when the process
    // starts, so there is no reason to hand a fresh container across the
    // boundary on every call — and one less owning container in the ABI.
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> out;
        const auto importers = ir::ImporterRegistry::makeDefault();
        for (const auto &imp : importers.importers()) {
            appendUnique(out, imp->formatName());
        }
        return out;
    }();
    static const std::vector<std::string_view> views{owned.begin(), owned.end()};
    return views;
}

std::span<const std::string_view> semanticWriteFormats() {
    static const std::vector<std::string> owned = [] {
        std::vector<std::string> out;
        const auto exporters = ir::SemanticExporterRegistry::makeDefault();
        for (const auto &exp : exporters.exporters())
            appendUnique(out, exp->formatName());
        return out;
    }();
    static const std::vector<std::string_view> views{owned.begin(), owned.end()};
    return views;
}

void write(const SemanticScene &handle, const std::filesystem::path &path,
           const SemanticWriteOptions &options) {
    const auto &scene = api::sceneOrThrow(handle, "write");
    const auto registry = ir::SemanticExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        throw Error{
            codes::kFatalExportWriteFailed,
            options.format.empty()
                ? std::format("no exporter claims the extension of '{}'", path.string())
                : std::format("no exporter named '{}' in this build; see semanticWriteFormats()",
                              options.format),
            path.string()};
    }
    try {
        exporter->write(scene, path, ir::SemanticExportConfig{options.compressionLevel});
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed, path.string());
    }
}

} // namespace nodehammer

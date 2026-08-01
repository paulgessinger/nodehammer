#include <api/handles.hpp>

#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/semantic/exporter.hpp>
#include <ir/semantic/importer.hpp>

#if NH_WITH_TGEO
#include <ir/tgeo/semantic/importer.hpp>
#endif

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

namespace nodehammer {
namespace {

using api::Access;

/// The shape every failing read returns: no scene, one error saying why.
[[nodiscard]] SemanticResult failure(std::string_view code, std::string_view message,
                                     std::string_view context = {}) {
    return SemanticResult{SemanticScene{}, Access::error(code, message, context)};
}

/// Adopt an internal ImportResult, scene and all.
///
/// Partial success is preserved on purpose: `ImportResult` documents that the
/// scene may be populated even when `diags.hasErrors()`, and suppressing it here
/// would decide on the caller's behalf that a partly-imported geometry is worth
/// nothing. The diagnostics are what say whether to trust the result; `valid()`
/// only says whether there is one to look at.
[[nodiscard]] SemanticResult adopt(ir::ImportResult result) {
    return SemanticResult{Access::wrap(std::move(result.scene)), Access::wrap(result.diags)};
}

void appendUnique(std::vector<std::string> &out, std::string_view name) {
    if (std::find(out.begin(), out.end(), name) == out.end()) {
        out.emplace_back(name);
    }
}

} // namespace

SemanticResult SemanticScene::read(const std::filesystem::path &path, const ReadOptions &options) {
    const auto registry = ir::ImporterRegistry::makeDefault();
    const auto *importer = registry.resolve(path, options.format);
    if (importer == nullptr) {
        // The string-dispatched entry point necessarily fails here rather than
        // at link time: "dd4hep" is a value, not a type, so nothing earlier
        // could have known this build lacks the backend (#41 §5).
        return failure(codes::kErrImportFormatUnknown,
                       options.format.empty()
                           ? std::format("no importer claims the extension of '{}'", path.string())
                           : std::format("no importer named '{}' in this build; see formats()",
                                         options.format),
                       path.string());
    }
    try {
        return adopt(importer->import(path));
    } catch (const std::exception &e) {
        return failure(codes::kErrImportFileNotFound, e.what(), path.string());
    }
}

SemanticResult SemanticScene::read(std::span<const std::byte> nhb) {
    try {
        return adopt(ir::FlatBufferImporter::importFromBytes("<memory>.nhb", nhb));
    } catch (const std::exception &e) {
        return failure(codes::kErrImportFileNotFound, e.what());
    }
}

#if NH_WITH_TGEO
SemanticResult SemanticScene::read(TGeoManager &manager) {
    try {
        return adopt(ir::TGeoImporter{}.import(&manager));
    } catch (const std::exception &e) {
        return failure(codes::kErrTgeoOpenFailed, e.what());
    }
}
#endif

std::vector<std::string> SemanticScene::formats() {
    std::vector<std::string> out;
    const auto importers = ir::ImporterRegistry::makeDefault();
    for (const auto &imp : importers.importers()) {
        appendUnique(out, imp->formatName());
    }
    const auto exporters = ir::SemanticExporterRegistry::makeDefault();
    for (const auto &exp : exporters.exporters()) {
        appendUnique(out, exp->formatName());
    }
    return out;
}

DiagnosticList SemanticScene::write(const std::filesystem::path &path,
                                    const WriteOptions &options) const {
    const auto *scene = Access::sceneOf(*this);
    if (scene == nullptr) {
        return Access::error(codes::kErrApiInvalidHandle, "cannot write an empty scene handle",
                             path.string());
    }
    const auto registry = ir::SemanticExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        return Access::error(
            codes::kErrExportWriteFailed,
            options.format.empty()
                ? std::format("no exporter claims the extension of '{}'", path.string())
                : std::format("no exporter named '{}' in this build; see formats()",
                              options.format),
            path.string());
    }
    try {
        return Access::wrap(exporter->write(*scene, path, ir::SemanticExportConfig{}).diags);
    } catch (const std::exception &e) {
        return Access::error(codes::kErrExportWriteFailed, e.what(), path.string());
    }
}

std::vector<std::byte> SemanticScene::toNhb() const {
    const auto *scene = Access::sceneOf(*this);
    if (scene == nullptr) {
        return {};
    }
    try {
        return ir::semanticSceneToBytes(*scene);
    } catch (const std::exception &) {
        return {};
    }
}

bool SemanticScene::valid() const noexcept { return Access::sceneOf(*this) != nullptr; }

std::size_t SemanticScene::nodeCount() const noexcept {
    const auto *scene = Access::sceneOf(*this);
    return scene != nullptr ? scene->nodes.size() : 0;
}

std::size_t SemanticScene::logVolCount() const noexcept {
    const auto *scene = Access::sceneOf(*this);
    return scene != nullptr ? scene->logVols.size() : 0;
}

std::size_t SemanticScene::shapeCount() const noexcept {
    const auto *scene = Access::sceneOf(*this);
    return scene != nullptr ? scene->shapes.size() : 0;
}

std::size_t SemanticScene::materialCount() const noexcept {
    const auto *scene = Access::sceneOf(*this);
    return scene != nullptr ? scene->materials.size() : 0;
}

} // namespace nodehammer

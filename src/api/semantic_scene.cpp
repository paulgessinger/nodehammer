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

/// Adopt an internal ImportResult.
///
/// An importer reports "could not read this" as an error *diagnostic* rather
/// than by throwing — all six such sites in the tree do, and every one of them
/// returns an empty scene alongside it. At the boundary that is an input error
/// like any other, so it becomes an exception here and `read` either hands back
/// a scene or explains why it cannot.
///
/// `ImportResult` does permit a populated scene alongside errors. Nothing
/// produces one, and an importer that wanted to report a *lossy* import already
/// has the channel for it: warnings plus the `DegradationFlags` the IR carries
/// per node. Errors mean there is nothing to hand back.
[[nodiscard]] SemanticResult adopt(ir::ImportResult result, std::string_view context) {
    if (result.diags.hasErrors()) {
        api::throwReportedErrors(result.diags, codes::kErrImportFileNotFound, context);
    }
    return SemanticResult{api::asHandle(std::move(result.scene)),
                          api::asHandle(std::move(result.diags))};
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
        throw Error{codes::kErrImportFormatUnknown,
                    options.format.empty()
                        ? std::format("no importer claims the extension of '{}'", path.string())
                        : std::format("no importer named '{}' in this build; see formats()",
                                      options.format),
                    path.string()};
    }
    try {
        return adopt(importer->import(path), path.string());
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrImportFileNotFound, path.string());
    }
}

SemanticResult SemanticScene::read(std::span<const std::byte> nhb) {
    try {
        return adopt(ir::FlatBufferImporter::importFromBytes("<memory>.nhb", nhb), "<memory>.nhb");
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrImportFileNotFound, "<memory>.nhb");
    }
}

#if NH_WITH_TGEO
SemanticResult SemanticScene::read(TGeoManager &manager) {
    try {
        return adopt(ir::TGeoImporter{}.import(&manager), "TGeoManager");
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrTgeoOpenFailed);
    }
}
#endif

std::span<const std::string_view> SemanticScene::formats() {
    // Built once and kept: which formats a build has is fixed when the process
    // starts, so there is no reason to hand a fresh container across the
    // boundary on every call — and one less owning container in the ABI.
    static const std::vector<std::string> owned = [] {
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
    }();
    static const std::vector<std::string_view> views{owned.begin(), owned.end()};
    return views;
}

DiagnosticList SemanticScene::write(const std::filesystem::path &path,
                                    const WriteOptions &options) const {
    const auto &scene = api::sceneOrThrow(*this, "SemanticScene::write");
    const auto registry = ir::SemanticExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        throw Error{codes::kErrExportWriteFailed,
                    options.format.empty()
                        ? std::format("no exporter claims the extension of '{}'", path.string())
                        : std::format("no exporter named '{}' in this build; see formats()",
                                      options.format),
                    path.string()};
    }
    try {
        auto result = exporter->write(scene, path, ir::SemanticExportConfig{});
        // Exporters, like importers, report "could not write this" as an error
        // diagnostic. Nothing was produced, so it becomes the exception.
        if (result.diags.hasErrors()) {
            api::throwReportedErrors(result.diags, codes::kErrExportWriteFailed, path.string());
        }
        return api::asHandle(std::move(result.diags));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrExportWriteFailed, path.string());
    }
}

std::vector<std::byte> SemanticScene::toNhb() const {
    const auto &scene = api::sceneOrThrow(*this, "SemanticScene::toNhb");
    try {
        return ir::semanticSceneToBytes(scene);
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrExportWriteFailed);
    }
}

bool SemanticScene::valid() const noexcept { return impl_ != nullptr; }

// The observers are members, so they read the state rather than going through
// a helper to ask whether there is any: `api::sceneOrThrow` is for the verbs, which
// are not members and have a caller to name.

std::size_t SemanticScene::nodeCount() const noexcept {
    return impl_ ? impl_->scene.nodes.size() : 0;
}

std::size_t SemanticScene::logVolCount() const noexcept {
    return impl_ ? impl_->scene.logVols.size() : 0;
}

std::size_t SemanticScene::shapeCount() const noexcept {
    return impl_ ? impl_->scene.shapes.size() : 0;
}

std::size_t SemanticScene::materialCount() const noexcept {
    return impl_ ? impl_->scene.materials.size() : 0;
}

} // namespace nodehammer

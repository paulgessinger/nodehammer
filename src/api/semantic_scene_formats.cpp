// The `SemanticScene` entry points that go through a format registry.
//
// Split from semantic_scene.cpp along one line: whether the call has to consult
// a registry to learn what to do. `read(TGeoManager &)` does not -- its
// argument's type settles the format -- while `read(path)` cannot know until it
// has looked at the path, and `formats()` exists only to report what the
// registries hold.
//
// The split is what lets the amalgamated connector header exist (docs/
// event-display-design.md §7). Registry dispatch reaches every importer and
// exporter this build has, and through the `.nhb` reader it reaches zstd -- a
// compiled C library, and the one dependency in this slice that cannot be
// pasted into a header. The direct entry points reach none of it: the TGeo and
// DD4hep importers and the FlatBuffer *serializer* are all header-inlinable,
// so a connector built from semantic_scene.cpp alone has no compiled
// dependency at all and needs nothing on the consumer's link line.
//
// Nothing here is conditional. Both files are in every normal build, and a
// consumer of the library sees one class whose members happen to be defined in
// two translation units.

#include <api/adopt.hpp>
#include <api/handles.hpp>

#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/importer.hpp>
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

SemanticResult SemanticScene::read(const std::filesystem::path &path, const ReadOptions &options) {
    const auto registry = ir::ImporterRegistry::makeDefault();
    const auto *importer = registry.resolve(path, options.format);
    if (importer == nullptr) {
        // The string-dispatched entry point necessarily fails here rather than
        // at link time: "dd4hep" is a value, not a type, so nothing earlier
        // could have known this build lacks the backend (#41 §5).
        throw Error{codes::kFatalImportFormatUnknown,
                    options.format.empty()
                        ? std::format("no importer claims the extension of '{}'", path.string())
                        : std::format("no importer named '{}' in this build; see formats()",
                                      options.format),
                    path.string()};
    }
    try {
        return api::adopt(importer->import(path));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, path.string());
    }
}

SemanticResult SemanticScene::read(std::span<const std::byte> nhb) {
    try {
        return api::adopt(ir::FlatBufferImporter::importFromBytes("<memory>.nhb", nhb));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, "<memory>.nhb");
    }
}

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

void SemanticScene::write(const std::filesystem::path &path, const WriteOptions &options) const {
    const auto &scene = api::sceneOrThrow(*this, "SemanticScene::write");
    const auto registry = ir::SemanticExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        throw Error{codes::kFatalExportWriteFailed,
                    options.format.empty()
                        ? std::format("no exporter claims the extension of '{}'", path.string())
                        : std::format("no exporter named '{}' in this build; see formats()",
                                      options.format),
                    path.string()};
    }
    try {
        exporter->write(scene, path, ir::SemanticExportConfig{});
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed, path.string());
    }
}

} // namespace nodehammer

#include <nodehammer/nhb.hpp>
#if NH_WITH_TGEO
#include <nodehammer/tgeo.hpp>
#endif
#if NH_WITH_DD4HEP
#include <nodehammer/dd4hep.hpp>
#endif
// In-memory geometry ingestion and .nhb transport, without format registries.

#include <api/handles_semantic.hpp>

#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>

#if NH_WITH_TGEO
#include <ir/tgeo/semantic/importer.hpp>
#endif

#if NH_WITH_DD4HEP
#include <ir/dd4hep/semantic/importer.hpp>
#endif

#include <exception>
#include <utility>

namespace nodehammer {

#if NH_WITH_TGEO
SemanticResult fromTGeo(TGeoManager &manager) {
    try {
        return api::asHandle(ir::TGeoImporter{}.import(&manager));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalTgeoOpenFailed);
    }
}
#endif

#if NH_WITH_DD4HEP
SemanticResult fromDD4hep(dd4hep::Detector &detector) {
    try {
        return api::asHandle(ir::DD4hepImporter{}.import(detector));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalTgeoOpenFailed);
    }
}
#endif

SemanticResult fromNhb(std::span<const std::byte> nhb) {
    try {
        return api::asHandle(ir::FlatBufferImporter::importFromBytes("<memory>.nhb", nhb));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, "<memory>.nhb");
    }
}

std::vector<std::byte> toNhb(const SemanticScene &handle) {
    const auto &scene = api::sceneOrThrow(handle, "toNhb");
    try {
        return ir::semanticSceneToBytes(scene);
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed);
    }
}

std::vector<std::byte> toNhbZstd(const SemanticScene &handle, int compressionLevel) {
    const auto &scene = api::sceneOrThrow(handle, "toNhbZstd");
    try {
        return detail::zstd_io::compress(ir::semanticSceneToBytes(scene), compressionLevel);
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed);
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

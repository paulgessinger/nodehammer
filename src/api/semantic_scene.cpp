// The `SemanticScene` entry points that need no format registry.
//
// An in-memory geometry handed over by its owner, and the bytes that come back
// out. Everything here reaches a named importer or the FlatBuffer serializer
// directly, so nothing in this file consults a registry or touches a
// filesystem -- see semantic_scene_formats.cpp for the other half, and for why
// the line is drawn there.

#include <api/adopt.hpp>
#include <api/handles.hpp>

#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>

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
SemanticResult SemanticScene::read(TGeoManager &manager) {
    try {
        return api::adopt(ir::TGeoImporter{}.import(&manager));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalTgeoOpenFailed);
    }
}
#endif

#if NH_WITH_DD4HEP
SemanticResult SemanticScene::read(dd4hep::Detector &detector) {
    try {
        return api::adopt(ir::DD4hepImporter{}.import(detector));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalTgeoOpenFailed);
    }
}
#endif

std::vector<std::byte> SemanticScene::toNhb() const {
    const auto &scene = api::sceneOrThrow(*this, "SemanticScene::toNhb");
    try {
        return ir::semanticSceneToBytes(scene);
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

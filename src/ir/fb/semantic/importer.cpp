#include <nodehammer/ir/fb/semantic/importer.hpp>

#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/fb/semantic/flatbuffer.hpp>

#include <format>

namespace nodehammer {

std::string_view FlatBufferImporter::formatName() const noexcept { return "flatbuffer"; }

std::vector<std::string> FlatBufferImporter::supportedExtensions() const {
    return {"nhb", "nhb.zst"};
}

ImportResult FlatBufferImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    try {
        auto raw = zstd_io::readBytesFromFile(path);
        auto scene = semanticSceneFromBytes(raw);
        scene.computeWorldTransforms();
        scene.computeOriginalPaths();
        result.scene = std::move(scene);
    } catch (const std::exception &ex) {
        result.diags.error(
            codes::kErrImportFileNotFound,
            std::format("failed to load FlatBuffer '{}': {}", path.string(), ex.what()));
    }

    return result;
}

} // namespace nodehammer

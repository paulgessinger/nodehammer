#include <nodehammer/import/flatbuffer_importer.hpp>

#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic_flatbuffer.hpp>

#include <format>

namespace nodehammer {

std::string_view FlatBufferImporter::formatName() const noexcept { return "flatbuffer"; }

std::vector<std::string> FlatBufferImporter::supportedExtensions() const { return {"nhb"}; }

ImportResult FlatBufferImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    try {
        auto raw = file_io::readFile(path);
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

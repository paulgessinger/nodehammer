#include <nodehammer/export/semantic_flatbuffer_exporter.hpp>

#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic_flatbuffer.hpp>

#include <format>

namespace nodehammer {

std::string_view SemanticFlatbufferExporter::formatName() const noexcept { return "nhb"; }

std::vector<std::string> SemanticFlatbufferExporter::supportedExtensions() const {
    return {"nhb", "nhb.zst"};
}

SemanticExportResult
SemanticFlatbufferExporter::write(const SemanticScene &scene, const std::filesystem::path &path,
                                  [[maybe_unused]] const SemanticExportConfig &config) const {
    SemanticExportResult result;

    try {
        auto bytes = semanticSceneToBytes(scene);
        zstd_io::writeBytesToFile(path, std::as_bytes(std::span{bytes}));
    } catch (const std::exception &ex) {
        result.diags.error(
            codes::kErrExportWriteFailed,
            std::format("failed to write FlatBuffer '{}': {}", path.string(), ex.what()),
            path.string());
    }

    return result;
}

} // namespace nodehammer

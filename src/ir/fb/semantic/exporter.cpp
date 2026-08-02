#include <ir/fb/semantic/exporter.hpp>

#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>

#include <format>
#include <span>

namespace nodehammer::ir {

std::string_view SemanticFlatbufferExporter::formatName() const noexcept { return "nhb"; }

std::vector<std::string> SemanticFlatbufferExporter::supportedExtensions() const {
    return {"nhb", "nhb.zst"};
}

SemanticExportResult
SemanticFlatbufferExporter::write(const semantic::Scene &scene, const std::filesystem::path &path,
                                  [[maybe_unused]] const SemanticExportConfig &config) const {
    SemanticExportResult result;

    try {
        auto bytes = semanticSceneToBytes(scene);
        detail::zstd_io::writeBytesToFile(path, std::as_bytes(std::span{bytes}));
    } catch (const std::exception &ex) {
        result.diags.error(
            codes::kFatalExportWriteFailed,
            std::format("failed to write FlatBuffer '{}': {}", path.string(), ex.what()),
            path.string());
    }

    return result;
}

} // namespace nodehammer::ir

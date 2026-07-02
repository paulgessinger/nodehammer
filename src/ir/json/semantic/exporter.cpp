#include <nodehammer/ir/json/semantic/exporter.hpp>

#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic_json.hpp>

#include <nlohmann/json.hpp>

#include <format>

namespace nodehammer {

std::string_view SemanticJsonExporter::formatName() const noexcept { return "json"; }

std::vector<std::string> SemanticJsonExporter::supportedExtensions() const {
    return {"json", "json.zst"};
}

SemanticExportResult
SemanticJsonExporter::write(const SemanticScene &scene, const std::filesystem::path &path,
                            [[maybe_unused]] const SemanticExportConfig &config) const {
    SemanticExportResult result;

    try {
        nlohmann::json j = scene;
        const std::string jsonStr = j.dump(-1);
        zstd_io::writeJsonToFile(path, jsonStr);
    } catch (const std::exception &ex) {
        result.diags.error(codes::kErrExportWriteFailed,
                           std::format("failed to write JSON '{}': {}", path.string(), ex.what()),
                           path.string());
    }

    return result;
}

} // namespace nodehammer

#include <ir/json/semantic/exporter.hpp>

#include <detail/zstd_io.hpp>
#include <ir/diagnostic_codes.hpp>
#include <ir/semantic_json.hpp>

#include <nlohmann/json.hpp>

#include <format>

namespace nodehammer::ir {

std::string_view SemanticJsonExporter::formatName() const noexcept { return "json"; }

std::vector<std::string> SemanticJsonExporter::supportedExtensions() const {
    return {"json", "json.zst"};
}

SemanticExportResult
SemanticJsonExporter::write(const semantic::Scene &scene, const std::filesystem::path &path,
                            [[maybe_unused]] const SemanticExportConfig &config) const {
    SemanticExportResult result;

    try {
        nlohmann::json j = scene;
        const std::string jsonStr = j.dump(-1);
        detail::zstd_io::writeJsonToFile(path, jsonStr);
    } catch (const std::exception &ex) {
        result.diags.error(codes::kErrExportWriteFailed,
                           std::format("failed to write JSON '{}': {}", path.string(), ex.what()),
                           path.string());
    }

    return result;
}

} // namespace nodehammer::ir

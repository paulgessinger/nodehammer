#include <nodehammer/import/json_importer.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>

namespace nodehammer {

std::string_view JsonImporter::formatName() const noexcept { return "json"; }

std::vector<std::string> JsonImporter::supportedExtensions() const { return {"json"}; }

ImportResult JsonImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    std::ifstream f{path};
    if (!f) {
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("failed to open JSON file '{}'", path.string()));
        return result;
    }

    try {
        auto j = nlohmann::json::parse(f);
        result.scene = j.get<SemanticScene>();
    } catch (const nlohmann::json::exception &ex) {
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("failed to parse JSON '{}': {}", path.string(), ex.what()));
    }

    return result;
}

} // namespace nodehammer

#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/json/semantic/importer.hpp>
#include <nodehammer/ir/semantic_json.hpp>

#include <nlohmann/json.hpp>

#include <format>

namespace nodehammer {

std::string_view JsonImporter::formatName() const noexcept { return "json"; }

std::vector<std::string> JsonImporter::supportedExtensions() const { return {"json", "json.zst"}; }

detail::ImportResult JsonImporter::import(const std::filesystem::path &path) const {
    detail::ImportResult result;

    try {
        auto jsonStr = zstd_io::readJsonFromFile(path);
        auto j = nlohmann::json::parse(jsonStr);
        result.scene = j.get<detail::SemanticScene>();
        result.scene.computeWorldTransforms();
        result.scene.computeOriginalPaths();
    } catch (const std::exception &ex) {
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("failed to load JSON '{}': {}", path.string(), ex.what()));
    }

    return result;
}

} // namespace nodehammer

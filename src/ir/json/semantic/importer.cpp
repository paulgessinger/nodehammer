#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/json/semantic/importer.hpp>
#include <ir/semantic_json.hpp>

#include <nlohmann/json.hpp>

#include <format>

namespace nodehammer::ir {

std::string_view JsonImporter::formatName() const noexcept { return "json"; }

std::vector<std::string> JsonImporter::supportedExtensions() const { return {"json", "json.zst"}; }

ImportResult JsonImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    try {
        auto jsonStr = detail::zstd_io::readJsonFromFile(path);
        auto j = nlohmann::json::parse(jsonStr);
        result.scene = j.get<semantic::Scene>();
        result.scene.computeWorldTransforms();
        result.scene.computeOriginalPaths();
    } catch (const Error &) {
        throw;
    } catch (const std::exception &ex) {
        throw Error{codes::kFatalImportFileNotFound,
                    std::format("failed to load JSON '{}': {}", path.string(), ex.what()),
                    path.string()};
    }

    return result;
}

} // namespace nodehammer::ir

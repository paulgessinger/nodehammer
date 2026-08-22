#include <detail/zstd_io.hpp>
#include <ir/render/json_exporter.hpp>
#include <ir/render_json.hpp>

#include <nlohmann/json.hpp>

namespace nodehammer::ir {

std::string_view RenderJsonExporter::formatName() const noexcept { return "render-json"; }

std::vector<std::string> RenderJsonExporter::supportedExtensions() const {
    return {".json", ".json.zst"};
}

void RenderJsonExporter::write(const render::Scene &scene, const std::filesystem::path &path,
                               const ExportConfig &config) const {
    // Indent two, as `dump-render` did. The other exporters read `config` for
    // unit and axis conventions; none of that applies to a dump of the IR as it
    // stands, which is the whole point of the format.
    (void)config;
    const nlohmann::json doc = scene;
    detail::zstd_io::writeJsonToFile(path, doc.dump(2));
}

} // namespace nodehammer::ir

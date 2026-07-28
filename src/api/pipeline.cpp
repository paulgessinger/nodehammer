#include <nodehammer/pipeline.hpp>

#include <nodehammer/detail/handle_seam.hpp>
#include <nodehammer/ir/semantic/importer.hpp>
#include <nodehammer/scene_build.hpp>

#include <string>
#include <utility>

namespace nodehammer {

ImportResult importGeometry(const std::filesystem::path &path, std::string_view format) {
    const auto registry = ImporterRegistry::makeDefault();
    const auto *importer = registry.resolve(path, format);
    if (importer == nullptr) {
        DiagnosticList diags;
        diags.error("NH0001",
                    format.empty()
                        ? "no importer claims extension '" + path.extension().string() +
                              "'; pass an explicit format"
                        : "no importer registered for format '" + std::string{format} + "'");
        return ImportResult{SemanticScene{}, std::move(diags)};
    }

    auto imported = importer->import(path);
    // The importer builds its scene by value; freezing it here by move is what
    // makes the handle's promise hold — no mutable alias survives the call.
    return ImportResult{detail::wrapSemanticScene(std::move(imported.scene)),
                        std::move(imported.diags)};
}

std::vector<std::string> importFormats() {
    const auto registry = ImporterRegistry::makeDefault();
    std::vector<std::string> names;
    names.reserve(registry.importers().size());
    for (const auto &importer : registry.importers()) {
        names.emplace_back(importer->formatName());
    }
    return names;
}

BuildResult buildScene(const std::filesystem::path &configPath,
                       const std::filesystem::path &geometryPath) {
    auto built = buildSceneFromPaths(configPath, geometryPath);
    // `built.scene` is null on any pipeline-stage failure; wrapRenderScene maps
    // that to an invalid handle, so failure is one check rather than two.
    return BuildResult{detail::wrapRenderScene(std::move(built.scene)), std::move(built.diags)};
}

} // namespace nodehammer

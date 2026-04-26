#include <nodehammer/scene_build.hpp>

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic/importer.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <utility>

namespace nodehammer {

SceneBuildResult build_scene_from_paths(const std::filesystem::path &config_path,
                                        const std::filesystem::path &input_path,
                                        std::optional<std::string> input_format) {
    DiagnosticList diags;

    NHConfig nhcfg;
    if (!config_path.empty()) {
        auto loaded = ConfigLoader::loadFromFile(config_path);
        diags.append(loaded.diags);
        if (loaded.diags.hasErrors()) {
            return {nullptr, std::move(diags)};
        }
        nhcfg = std::move(loaded.config);
        auto validDiags = ConfigValidator::validate(nhcfg);
        diags.append(validDiags);
        if (validDiags.hasErrors()) {
            return {nullptr, std::move(diags)};
        }
    }

    auto registry = ImporterRegistry::makeDefault();
    const std::string fmt = input_format.value_or(std::string{});
    const auto *imp = registry.resolve(input_path.string(), fmt);
    if (imp == nullptr) {
        diags.error(codes::kErrImportFormatUnknown,
                    std::string{"cannot determine input format for '"} + input_path.string() + "'",
                    input_path.string());
        return {nullptr, std::move(diags)};
    }
    auto importResult = imp->import(input_path);
    diags.append(importResult.diags);
    if (importResult.diags.hasErrors()) {
        return {nullptr, std::move(diags)};
    }

    if (!nhcfg.selection.empty()) {
        SelectionEngine sel{nhcfg.selection, nhcfg.hoistOrphans};
        auto selDiags = sel.prune(importResult.scene);
        diags.append(selDiags);
        if (selDiags.hasErrors()) {
            return {nullptr, std::move(diags)};
        }
    }

    if (nhcfg.deduplicateShapes) {
        importResult.scene.deduplicateMaterials();
        importResult.scene.deduplicateShapes();
        importResult.scene.deduplicateLogVols();
    }

    TessellationPass pass{nhcfg};
    auto tessResult = pass.lower(importResult.scene);
    diags.append(tessResult.diags);
    if (tessResult.diags.hasErrors()) {
        return {nullptr, std::move(diags)};
    }

    return {std::make_shared<RenderScene>(std::move(tessResult.scene)), std::move(diags)};
}

} // namespace nodehammer

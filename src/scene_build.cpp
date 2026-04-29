#include <nodehammer/scene_build.hpp>

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic/importer.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <utility>

namespace nodehammer {

namespace {

/// Path-based scene prep — used only by `buildSceneFromPaths` (CLI flow).
/// The viewer takes the byte-driven `prepareSceneForTessellationFromInputs`
/// path via BuildSession and never calls this.
ScenePrepResult
prepareSceneForTessellationFromPaths(const std::filesystem::path &config_path,
                                     const std::filesystem::path &input_path,
                                     const std::optional<std::string> &input_format) {
    ScenePrepResult prep;

    if (!config_path.empty()) {
        auto loaded = ConfigLoader::loadFromFile(config_path);
        prep.diags.append(loaded.diags);
        if (loaded.diags.hasErrors()) {
            return prep;
        }
        prep.config = std::move(loaded.config);
        auto validDiags = ConfigValidator::validate(prep.config);
        prep.diags.append(validDiags);
        if (validDiags.hasErrors()) {
            return prep;
        }
    }

    auto registry = ImporterRegistry::makeDefault();
    const std::string fmt = input_format.value_or(std::string{});
    const auto *imp = registry.resolve(input_path.string(), fmt);
    if (imp == nullptr) {
        prep.diags.error(codes::kErrImportFormatUnknown,
                         std::string{"cannot determine input format for '"} + input_path.string() +
                             "'",
                         input_path.string());
        return prep;
    }
    auto importResult = imp->import(input_path);
    prep.diags.append(importResult.diags);
    if (importResult.diags.hasErrors()) {
        return prep;
    }
    prep.scene = std::move(importResult.scene);

    if (!prep.config.selection.empty()) {
        SelectionEngine sel{prep.config.selection, prep.config.hoistOrphans};
        auto selDiags = sel.prune(prep.scene);
        prep.diags.append(selDiags);
        if (selDiags.hasErrors()) {
            return prep;
        }
    }

    if (prep.config.deduplicateShapes) {
        prep.scene.deduplicateMaterials();
        prep.scene.deduplicateShapes();
        prep.scene.deduplicateLogVols();
    }

    prep.ok = true;
    return prep;
}

} // namespace

ScenePrepResult prepareSceneForTessellationFromInputs(NHConfig config, SemanticScene scene) {
    ScenePrepResult prep;
    prep.config = std::move(config);
    prep.scene = std::move(scene);

    auto validDiags = ConfigValidator::validate(prep.config);
    prep.diags.append(validDiags);
    if (validDiags.hasErrors()) {
        return prep;
    }

    if (!prep.config.selection.empty()) {
        SelectionEngine sel{prep.config.selection, prep.config.hoistOrphans};
        auto selDiags = sel.prune(prep.scene);
        prep.diags.append(selDiags);
        if (selDiags.hasErrors()) {
            return prep;
        }
    }

    if (prep.config.deduplicateShapes) {
        prep.scene.deduplicateMaterials();
        prep.scene.deduplicateShapes();
        prep.scene.deduplicateLogVols();
    }

    prep.ok = true;
    return prep;
}

SceneBuildResult buildSceneFromPaths(const std::filesystem::path &config_path,
                                     const std::filesystem::path &input_path,
                                     const std::optional<std::string> &input_format) {
    auto prep = prepareSceneForTessellationFromPaths(config_path, input_path, input_format);
    if (!prep.ok) {
        return {nullptr, std::move(prep.diags)};
    }

    TessellationPass pass{prep.config};
    auto tessResult = pass.lower(prep.scene);
    prep.diags.append(tessResult.diags);
    if (tessResult.diags.hasErrors()) {
        return {nullptr, std::move(prep.diags)};
    }

    return {std::make_shared<RenderScene>(std::move(tessResult.scene)), std::move(prep.diags)};
}

} // namespace nodehammer

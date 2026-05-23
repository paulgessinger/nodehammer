#include <nodehammer/scene_build.hpp>

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic/importer.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace nodehammer {

ScenePrepResult prepareSceneForTessellationFromInputs(NHConfig config, SemanticScene scene,
                                                      std::optional<WedgeCutParams> wedgeCut) {
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

    // Azimuthal wedge cut runs after dedup so cut shapes that share an
    // identical local-frame cut stay instanced (mirrors the convert CLI).
    if (wedgeCut) {
        (void)applyWedgeCut(prep.scene, *wedgeCut, prep.diags);
    }

    prep.ok = true;
    return prep;
}

SceneBuildResult buildSceneFromPaths(const std::filesystem::path &config_path,
                                     const std::filesystem::path &geometry_path) {
    SceneBuildResult result;

    if (geometry_path.empty()) {
        result.diags.error(codes::kErrImportFormatUnknown,
                           "buildSceneFromPaths: input path is empty", "");
        return result;
    }

    NHConfig cfg;
    if (!config_path.empty()) {
        auto loaded = ConfigLoader::loadFromFile(config_path);
        result.diags.append(loaded.diags);
        if (loaded.diags.hasErrors()) {
            return result;
        }
        cfg = std::move(loaded.config);
    }

    const auto importerRegistry = ImporterRegistry::makeDefault();
    const auto *importer = importerRegistry.resolve(geometry_path);
    if (importer == nullptr) {
        result.diags.error(codes::kErrImportFormatUnknown,
                           "buildSceneFromPaths: no importer for '" + geometry_path.string() + "'",
                           geometry_path.string());
        return result;
    }

    auto importResult = importer->import(geometry_path);
    result.diags.append(importResult.diags);
    if (importResult.diags.hasErrors()) {
        return result;
    }

    auto prep =
        prepareSceneForTessellationFromInputs(std::move(cfg), std::move(importResult.scene));
    result.diags.append(prep.diags);
    if (!prep.ok) {
        return result;
    }

    TessellationPass pass{prep.config};
    auto tessResult = pass.lower(prep.scene);
    result.diags.append(tessResult.diags);
    if (tessResult.diags.hasErrors()) {
        return result;
    }

    result.scene = std::make_shared<RenderScene>(std::move(tessResult.scene));
    return result;
}

} // namespace nodehammer

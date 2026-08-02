#include <scene_build.hpp>

#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <diagnostic_codes.hpp>
#include <ir/semantic/importer.hpp>
#include <selection/selector.hpp>
#include <tessellation/build_pipeline.hpp>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace nodehammer::pipeline {

ScenePrepResult
prepareSceneForTessellationFromInputs(config::NHConfig config, ir::semantic::Scene scene,
                                      std::optional<tessellation::WedgeCutParams> wedgeCut) {
    ScenePrepResult prep;
    prep.config = std::move(config);
    prep.scene = std::move(scene);

    auto validDiags = config::ConfigValidator::validate(prep.config);
    prep.diags.append(validDiags);
    if (validDiags.hasErrors()) {
        return prep;
    }

    if (!prep.config.selection.empty()) {
        selection::SelectionEngine sel{prep.config.selection, prep.config.hoistOrphans};
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
        (void)tessellation::applyWedgeCut(prep.scene, *wedgeCut);
    }

    prep.ok = true;
    return prep;
}

SceneBuildResult buildSceneFromPaths(const std::filesystem::path &config_path,
                                     const std::filesystem::path &geometry_path) {
    SceneBuildResult result;

    if (geometry_path.empty()) {
        result.diags.error(codes::kFatalImportFormatUnknown,
                           "buildSceneFromPaths: input path is empty", "");
        return result;
    }

    config::NHConfig cfg;
    if (!config_path.empty()) {
        auto loaded = config::ConfigLoader::loadFromFile(config_path);
        result.diags.append(loaded.diags);
        if (loaded.diags.hasErrors()) {
            return result;
        }
        cfg = std::move(loaded.config);
    }

    const auto importerRegistry = ir::ImporterRegistry::makeDefault();
    const auto *importer = importerRegistry.resolve(geometry_path);
    if (importer == nullptr) {
        result.diags.error(codes::kFatalImportFormatUnknown,
                           "buildSceneFromPaths: no importer for '" + geometry_path.string() + "'",
                           geometry_path.string());
        return result;
    }

    auto importResult = importer->import(geometry_path);
    result.diags.append(importResult.diags);
    if (importResult.diags.hasErrors()) {
        return result;
    }

    // Drive the shared BuildPipeline to completion — the same prep → (wedge) →
    // tessellate core the viewer backends use, so this synchronous shim is no
    // longer a hand-copied 4th sequence. `buildSceneFromPaths` applies no wedge
    // (nullopt), and prep now runs with a deferred wedge like every other site
    // (invariant #1). A single unbounded slice finishes in one drive loop.
    tessellation::BuildPipeline pipe;
    pipe.start(std::make_shared<const config::NHConfig>(std::move(cfg)),
               std::make_shared<const ir::semantic::Scene>(std::move(importResult.scene)),
               std::nullopt);
    while (!pipe.advance(std::numeric_limits<std::uint64_t>::max())) {
    }
    SceneBuildResult built = pipe.take();
    result.diags.append(built.diags);
    result.scene = std::move(built.scene); // null when prep/tessellation failed
    return result;
}

} // namespace nodehammer::pipeline

#include <scene_build.hpp>

#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <diagnostic_codes.hpp>
#include <geometry/approximation.hpp>
#include <geometry/drift_chamber.hpp>
#include <geometry/subtract_daughters.hpp>
#include <ir/semantic/importer.hpp>
#include <selection/shared_scene.hpp>
#include <tessellation/build_pipeline.hpp>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace nodehammer::pipeline {

ScenePrepResult
prepareSceneForTessellationFromInputs(config::NHConfig config, const ir::semantic::Scene &source,
                                      std::optional<tessellation::WedgeCutParams> wedgeCut) {
    ScenePrepResult prep;
    prep.config = std::move(config);

    auto validDiags = config::ConfigValidator::validate(prep.config);
    prep.diags.append(validDiags);
    diagnostics::throwIfErrors(validDiags, "prepareSceneForTessellation");

    ir::semantic::Scene scene;
    std::optional<ir::semantic::Scene> carved;
    if (geometry::hasDaughterSubtraction(prep.config)) {
        carved = source;
        prep.diags.append(geometry::subtractDaughters(*carved, prep.config));
    }
    const auto &selectionSource = carved ? *carved : source;
    if (!prep.config.selection.empty()) {
        DiagnosticList selectionDiags;
        scene = ir::semantic::selectCopy(selectionSource, prep.config.selection,
                                         prep.config.hoistOrphans, selectionDiags);
        prep.diags.append(selectionDiags);
    } else {
        if (carved)
            scene = std::move(*carved);
        else
            scene = source;
    }

    auto chamberDiags = geometry::optimizeDriftChambers(scene, prep.config);
    prep.diags.append(chamberDiags);
    diagnostics::throwIfErrors(chamberDiags, "drift chamber optimization");

    auto approximationDiags = geometry::approximate(scene, prep.config);
    prep.diags.append(approximationDiags);
    diagnostics::throwIfErrors(approximationDiags, "geometry approximation");

    if (prep.config.deduplicateShapes) {
        scene.deduplicateMaterials();
        scene.deduplicateShapes();
        scene.deduplicateLogVols();
    }

    prep.scene = std::move(scene);

    // Azimuthal wedge cut runs after dedup so cut shapes that share an
    // identical local-frame cut stay instanced (mirrors the convert CLI).
    if (wedgeCut) {
        (void)tessellation::applyWedgeCut(prep.scene, *wedgeCut, prep.cutOverrides);
    }

    return prep;
}

SceneBuildResult buildSceneFromPaths(const std::filesystem::path &config_path,
                                     const std::filesystem::path &geometry_path,
                                     const SemanticReadOptions &options,
                                     std::optional<tessellation::WedgeCutParams> wedgeCut) {
    SceneBuildResult result;

    if (geometry_path.empty()) {
        throw Error{codes::kFatalImportFormatUnknown, "buildSceneFromPaths: input path is empty"};
    }

    config::NHConfig cfg;
    if (!config_path.empty()) {
        auto loaded = config::ConfigLoader::loadFromFile(config_path);
        result.diags.append(loaded.diags);
        cfg = std::move(loaded.config);
    }

    const auto importerRegistry = ir::ImporterRegistry::makeDefault(options);
    const auto *importer = importerRegistry.resolve(geometry_path, options.format);
    if (importer == nullptr) {
        throw Error{codes::kFatalImportFormatUnknown,
                    "buildSceneFromPaths: no importer for '" + geometry_path.string() + "'",
                    geometry_path.string()};
    }

    auto importResult = importer->import(geometry_path);
    result.diags.append(importResult.diags);

    // Drive the shared BuildPipeline to completion — the same prep → (wedge) →
    // tessellate core the viewer backends use, so this synchronous shim is no
    // longer a hand-copied sequence. Optional wedges follow the same deferred
    // path as viewer builds. A single unbounded slice finishes in one drive loop.
    tessellation::BuildPipeline pipe;
    pipe.start(std::make_shared<const config::NHConfig>(std::move(cfg)),
               std::make_shared<const ir::semantic::Scene>(std::move(importResult.scene)),
               wedgeCut);
    while (!pipe.advance(std::numeric_limits<std::uint64_t>::max())) {
    }
    SceneBuildResult built = pipe.take();
    result.diags.append(built.diags);
    result.failure = std::move(built.failure);
    result.scene = std::move(built.scene); // null when prep/tessellation failed
    return result;
}

} // namespace nodehammer::pipeline

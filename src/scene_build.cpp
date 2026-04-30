#include <nodehammer/scene_build.hpp>

#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/viewer/bag_project_fs.hpp>
#include <nodehammer/viewer/build_session.hpp>

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace nodehammer {

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
                                     const std::filesystem::path &geometry_path) {
    SceneBuildResult result;

    if (geometry_path.empty()) {
        result.diags.error(codes::kErrImportFormatUnknown,
                           "buildSceneFromPaths: input path is empty", "");
        return result;
    }

    viewer::BagProjectFs bag;
    std::string config_key;
    if (!config_path.empty()) {
        try {
            auto bytes = file_io::readFile(config_path);
            config_key = config_path.filename().string();
            bag.addBytes(config_key, std::span<const std::byte>{bytes});
        } catch (const std::exception &e) {
            result.diags.error(codes::kErrImportFormatUnknown, e.what(), config_path.string());
            return result;
        }
    }

    std::string geometry_key = geometry_path.filename().string();
    try {
        auto bytes = file_io::readFile(geometry_path);
        bag.addBytes(geometry_key, std::span<const std::byte>{bytes});
    } catch (const std::exception &e) {
        result.diags.error(codes::kErrImportFormatUnknown, e.what(), geometry_path.string());
        return result;
    }

    viewer::BuildSession session;
    session.setRootKeys(config_key, geometry_key);

    while (true) {
        session.poll(&bag);
        const auto phase = session.phase();
        if (phase == viewer::BuildPhase::ResolvedReady) {
            break;
        }
        if (phase == viewer::BuildPhase::Error) {
            result.diags.error(codes::kErrImportFormatUnknown, session.errorMessage(),
                               geometry_path.string());
            return result;
        }
        if (phase == viewer::BuildPhase::WaitingForUser) {
            std::string missing_list;
            for (const auto &k : session.missing()) {
                if (!missing_list.empty()) {
                    missing_list += ", ";
                }
                missing_list += k;
            }
            result.diags.error(codes::kErrImportFormatUnknown,
                               "buildSceneFromPaths: missing files: " + missing_list,
                               geometry_path.string());
            return result;
        }
        // Walking / Idle / Consumed: keep polling. The bag is synchronous,
        // so settle happens within a couple of polls.
    }

    auto inputs = session.takeInputs();
    if (!inputs) {
        result.diags.error(codes::kErrImportFormatUnknown,
                           "buildSceneFromPaths: BuildSession produced no inputs",
                           geometry_path.string());
        return result;
    }

    result.diags.append(inputs->config.diags);
    result.diags.append(inputs->import.diags);
    if (inputs->config.diags.hasErrors() || inputs->import.diags.hasErrors()) {
        return result;
    }

    auto prep = prepareSceneForTessellationFromInputs(std::move(inputs->config.config),
                                                      std::move(inputs->import.scene));
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

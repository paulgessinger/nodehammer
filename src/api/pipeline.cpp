#include <nodehammer/pipeline.hpp>

#include <nodehammer/scene_build.hpp>

#include <utility>

namespace nodehammer {

BuildResult buildScene(const std::filesystem::path &configPath,
                       const std::filesystem::path &geometryPath) {
    auto built = buildSceneFromPaths(configPath, geometryPath);
    // `built.scene` is null on any pipeline-stage failure; wrapRenderScene maps
    // that to an invalid handle, so failure is one check rather than two.
    return BuildResult{wrapRenderScene(std::move(built.scene)), std::move(built.diags)};
}

} // namespace nodehammer

// Same prepared input, either direct canonical lowering or the former expand + lower path.
#include <chrono>
#include <config/config_loader.hpp>
#include <fstream>
#include <iostream>
#include <ir/expanded/conversion.hpp>
#include <ir/render_json.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <nlohmann/json.hpp>
#include <scene_build.hpp>
#include <sys/resource.h>
#include <tessellation/tessellation_pass.hpp>

int main(int argc, char **argv) {
    if ((argc != 4 && argc != 5) ||
        (std::string_view{argv[3]} != "canonical" && std::string_view{argv[3]} != "expanded")) {
        std::cerr << "usage: nodehammer_tessellation_bench INPUT CONFIG canonical|expanded "
                     "[RENDER_JSON]\n";
        return 2;
    }
    using namespace nodehammer;
    auto imported = ir::semantic::readFlatbuffer(argv[1]);
    auto cfg = config::ConfigLoader::loadFromFile(argv[2]);
    auto prep =
        pipeline::prepareSceneForTessellationFromInputs(std::move(cfg.config), std::move(imported));
    const auto start = std::chrono::steady_clock::now();
    tessellation::TessellationPass pass{prep.config};
    const auto result = std::string_view{argv[3]} == "canonical"
                            ? pass.lower(prep.scene)
                            : pass.lower(ir::semantic::expand(prep.scene));
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    uint64_t rss = static_cast<uint64_t>(usage.ru_maxrss);
#ifndef __APPLE__
    rss *= 1024;
#endif
    uint64_t triangles = 0;
    for (const auto &[id, mesh] : result.scene.meshAssets) {
        (void)id;
        triangles += mesh.indices.size() / 3;
    }
    std::cout << nlohmann::json{{"mode", argv[3]},
                                {"milliseconds", elapsed},
                                {"peak_rss_bytes", rss},
                                {"nodes", result.scene.nodes.size()},
                                {"meshes", result.scene.meshAssets.size()},
                                {"triangles", triangles},
                                {"errors", result.diags.hasErrors()}}
                     .dump()
              << '\n';
    // Optional exact render comparison, deliberately outside both measurements.
    if (argc == 5) {
        std::ofstream output(argv[4]);
        output << nlohmann::json(result.scene).dump();
        if (!output) {
            return 2;
        }
    }
    return result.diags.hasErrors() ? 1 : 0;
}

// Same prepared input, either direct canonical lowering or the former expand + lower path.
#include <bit>
#include <chrono>
#include <config/config_loader.hpp>
#include <fstream>
#include <iostream>
#include <ir/expanded/conversion.hpp>
#include <ir/render_json.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <scene_build.hpp>
#include <sys/resource.h>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>

int main(int argc, char **argv) {
    if ((argc != 4 && argc != 5) ||
        (std::string_view{argv[3]} != "canonical" && std::string_view{argv[3]} != "expanded" &&
         std::string_view{argv[3]} != "canonical-cut" &&
         std::string_view{argv[3]} != "expanded-cut")) {
        std::cerr << "usage: nodehammer_tessellation_bench INPUT CONFIG "
                     "canonical|expanded|canonical-cut|expanded-cut "
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
    const std::string_view mode{argv[3]};
    const bool cut = mode.ends_with("-cut");
    tessellation::TessellationPassResult result;
    double cutMilliseconds = 0.0;
    if (mode.starts_with("canonical")) {
        tessellation::OccurrenceOverrides overrides;
        if (cut) {
            (void)tessellation::applyWedgeCut(prep.scene, {0.0, 163.0}, overrides);
        }
        cutMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        result = pass.lower(prep.scene, &overrides);
    } else {
        auto physical = ir::semantic::expand(prep.scene);
        prep.scene = {};
        if (cut) {
            (void)tessellation::applyWedgeCut(physical, {0.0, 163.0});
        }
        cutMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        result = pass.lower(physical);
    }
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
    // Full vertex/normal/index checksum, outside the timed interval. Render
    // JSON alone contains mesh counts, so cannot establish geometry parity.
    uint64_t meshHash = 14695981039346656037ULL;
    auto mix = [&](uint32_t word) { meshHash = (meshHash ^ word) * 1099511628211ULL; };
    std::map<ir::render::MeshAssetId, const ir::render::MeshAsset *> ordered;
    for (const auto &[id, mesh] : result.scene.meshAssets) {
        ordered.emplace(id, &mesh);
    }
    for (const auto &[id, mesh] : ordered) {
        (void)id;
        for (const auto &vertex : mesh->vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                mix(std::bit_cast<uint32_t>(vertex.position[axis]));
                mix(std::bit_cast<uint32_t>(vertex.normal[axis]));
            }
        }
        for (const auto index : mesh->indices) {
            mix(index);
        }
    }
    std::cout << nlohmann::json{{"mode", argv[3]},
                                {"milliseconds", elapsed},
                                {"pre_tessellation_ms", cutMilliseconds},
                                {"peak_rss_bytes", rss},
                                {"nodes", result.scene.nodes.size()},
                                {"meshes", result.scene.meshAssets.size()},
                                {"mesh_buffers_hash", meshHash},
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

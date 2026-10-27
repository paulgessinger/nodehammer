// Minimal harness for the CI benchmark. Mirrors a `nodehammer convert -c CFG
// -i IN -o OUT` invocation with default options — no CLI11, no subcommand
// registry — so we can ship a small wasm bundle (Node-only, NODERAWFS) and
// time the pipeline without the full CLI surface.
//
// Usage: nodehammer_bench <config.toml> <input> <output>
//
// Exit code is non-zero on any pipeline-stage error; diagnostics go to stderr.

#include <config/config_loader.hpp>
#include <export_resolve.hpp>
#include <ir/render/exporter.hpp>
#include <ir/semantic/importer.hpp>
#include <scene_build.hpp>
#include <tessellation/build_pipeline.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <print>
#include <string>

namespace {

void printDiags(const nodehammer::DiagnosticList &diags) {
    for (const auto &d : diags.items()) {
        const char *sev = d.severity >= nodehammer::diagnostics::Severity::Error     ? "error"
                          : d.severity == nodehammer::diagnostics::Severity::Warning ? "warning"
                                                                                     : "info";
        std::println(std::cerr, "[{}] {} {}", sev, d.code, d.message);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4 && !(argc == 7 && std::string_view{argv[4]} == "--wedge-cut")) {
        std::println(std::cerr, "usage: {} <config.toml> <input> <output> [--wedge-cut START END]",
                     argc > 0 ? argv[0] : "nodehammer_bench");
        return 2;
    }

    const std::filesystem::path config_path = argv[1];
    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];

    std::optional<nodehammer::tessellation::WedgeCutParams> wedge;
    if (argc == 7) {
        try {
            auto angle = [](const char *text) {
                std::size_t used = 0;
                const std::string value{text};
                const double result = std::stod(value, &used);
                if (used != value.size() || !std::isfinite(result))
                    throw std::invalid_argument{"invalid angle"};
                return result;
            };
            wedge = nodehammer::tessellation::WedgeCutParams{angle(argv[5]), angle(argv[6])};
        } catch (const std::exception &e) {
            std::println(std::cerr, "bench: invalid wedge angles: {}", e.what());
            return 2;
        }
    }
    // Opt-in JSON-lines timing output also survives a timeout during a later stage.
    auto build = [&]() {
        if (!std::getenv("NH_BENCH_TIMING"))
            return nodehammer::pipeline::buildSceneFromPaths(config_path, input_path, {}, wedge);
        using Clock = std::chrono::steady_clock;
        auto last = Clock::now();
        auto record = [&](std::string_view stage) {
            const auto now = Clock::now();
            std::println("{{\"stage\":\"{}\",\"seconds\":{:.6f}}}", stage,
                         std::chrono::duration<double>(now - last).count());
            std::fflush(stdout);
            last = now;
        };
        auto loaded = nodehammer::config::ConfigLoader::loadFromFile(config_path);
        printDiags(loaded.diags);
        nodehammer::diagnostics::throwIfErrors(loaded.diags, "benchmark config");
        record("config");
        const auto importers = nodehammer::ir::ImporterRegistry::makeDefault();
        const auto *importer = importers.resolve(input_path);
        if (!importer)
            throw std::invalid_argument{"unsupported input format"};
        auto imported = importer->import(input_path);
        printDiags(imported.diags);
        record("import");
        nodehammer::tessellation::BuildPipeline pipe;
        pipe.start(
            std::make_shared<const nodehammer::config::NHConfig>(std::move(loaded.config)),
            std::make_shared<const nodehammer::ir::semantic::Scene>(std::move(imported.scene)),
            wedge);
        auto phase = pipe.phase();
        bool done = false;
        do {
            done = pipe.advance();
            if (pipe.phase() != phase) {
                constexpr std::array names{
                    "idle", "prepare", "classify_cut", "tessellate", "finalize",
                    "done", "queue",   "prepare_cut",  "bounds",     "prepare_tessellation",
                    "prune"};
                record(names.at(static_cast<std::size_t>(phase)));
                phase = pipe.phase();
            }
        } while (!done);
        return pipe.take();
    };
    auto built = build();
    printDiags(built.diags);
    if (built.failure)
        std::println(std::cerr, "bench: {}", built.failure->what());
    if (!built.scene || built.diags.hasErrors()) {
        std::println(std::cerr, "bench: scene build failed");
        return 1;
    }

    const auto registry = nodehammer::ir::RenderExporterRegistry::makeDefault();
    const auto *exp = registry.resolve(output_path.string(), {});
    if (exp == nullptr) {
        std::println(std::cerr, "bench: cannot determine output format for '{}'",
                     output_path.string());
        return 1;
    }

    // Re-read the config for its `[export.*]` tables: `buildSceneFromPaths`
    // consumes the parsed config internally and hands back only the scene. The
    // load already succeeded above (the build depends on it), so its
    // diagnostics have been reported and are dropped here rather than printed
    // twice. Without this the bench silently ignored `[export.*]` and exported
    // at the format defaults — a different scale from `convert` on the same
    // config.
    const auto reloaded = nodehammer::config::ConfigLoader::loadFromFile(config_path);
    const auto ecfg = nodehammer::pipeline::resolveExportConfig(reloaded.config, output_path);

    exp->write(*built.scene, output_path.string(), ecfg);

    return 0;
}

// Minimal harness for the CI benchmark. Mirrors a `nodehammer convert -c CFG
// -i IN -o OUT` invocation with default options — no CLI11, no subcommand
// registry — so we can ship a small wasm bundle (Node-only, NODERAWFS) and
// time the pipeline without the full CLI surface.
//
// Usage: nodehammer_bench <config.toml> <input> <output>
//
// Exit code is non-zero on any pipeline-stage error; diagnostics go to stderr.

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/export_resolve.hpp>
#include <nodehammer/ir/render/exporter.hpp>
#include <nodehammer/scene_build.hpp>

#include <filesystem>
#include <iostream>
#include <print>

namespace {

void printDiags(const nodehammer::DiagnosticList &diags) {
    for (const auto &d : diags.items()) {
        const char *sev = d.severity >= nodehammer::DiagnosticSeverity::Error     ? "error"
                          : d.severity == nodehammer::DiagnosticSeverity::Warning ? "warning"
                                                                                  : "info";
        std::println(std::cerr, "[{}] {} {}", sev, d.code, d.message);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::println(std::cerr, "usage: {} <config.toml> <input> <output>",
                     argc > 0 ? argv[0] : "nodehammer_bench");
        return 2;
    }

    const std::filesystem::path config_path = argv[1];
    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];

    auto built = nodehammer::buildSceneFromPaths(config_path, input_path);
    printDiags(built.diags);
    if (!built.scene) {
        std::println(std::cerr, "bench: scene build failed");
        return 1;
    }

    const auto registry = nodehammer::RenderExporterRegistry::makeDefault();
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
    const auto reloaded = nodehammer::ConfigLoader::loadFromFile(config_path);
    const auto ecfg = nodehammer::resolveExportConfig(reloaded.config, output_path);

    auto expResult = exp->write(*built.scene, output_path.string(), ecfg);
    printDiags(expResult.diags);
    if (expResult.diags.hasErrors()) {
        std::println(std::cerr, "bench: export to '{}' failed", output_path.string());
        return 1;
    }

    return 0;
}

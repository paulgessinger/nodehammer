#include <CLI/CLI.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/export/exporter.hpp>
#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <print>
#include <string>

namespace {

void printDiags(const nodehammer::DiagnosticList &diags) {
    for (const auto &d : diags.items()) {
        const char *sev = (d.severity >= nodehammer::DiagnosticSeverity::Error)     ? "error"
                          : (d.severity == nodehammer::DiagnosticSeverity::Warning) ? "warn"
                                                                                    : "info";
        std::println(stderr, "[{}] {} {}", sev, d.code, d.message);
    }
}

} // namespace

void register_cmd_convert(CLI::App &app) {
    auto *sub = app.add_subcommand("convert", "Convert a geometry file to a render format");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file")->required();
    auto *fmtInOpt =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *configOpt = sub->add_option("-c,--config", "TOML config file");
    auto *outputOpt = sub->add_option("-o,--output", "Output file(s) — one per format")
                          ->required()
                          ->expected(1, -1);
    auto *fmtOutOpt = sub->add_option(
        "--output-format",
        "Output format (auto-detected from extension if omitted; applies to all outputs)");
    auto *strictOpt = sub->add_flag("--strict", "Treat warnings as errors");

    sub->callback([=] {
        std::string inputPath, inputFmt, outputFmt;
        std::vector<std::string> outputPaths;
        inputOpt->results(inputPath);
        outputOpt->results(outputPaths);
        if (*fmtInOpt) {
            fmtInOpt->results(inputFmt);
        }
        if (*fmtOutOpt) {
            fmtOutOpt->results(outputFmt);
        }
        const bool strict = strictOpt->count() > 0;

        // ── Load config ────────────────────────────────────────────────────────
        nodehammer::NHConfig cfg;
        if (*configOpt) {
            std::string cfgPath;
            configOpt->results(cfgPath);
            auto loaded = nodehammer::ConfigLoader::loadFromFile(cfgPath);
            printDiags(loaded.diags);
            if (loaded.diags.hasErrors()) {
                std::println(stderr, "convert: config load failed");
                std::exit(1);
            }
            cfg = std::move(loaded.config);

            auto validDiags = nodehammer::ConfigValidator::validate(cfg);
            printDiags(validDiags);
            if (validDiags.hasErrors()) {
                std::println(stderr, "convert: config validation failed");
                std::exit(1);
            }
        }

        // ── Import ─────────────────────────────────────────────────────────────
        const auto impRegistry = nodehammer::makeDefaultRegistry();
        const auto *imp = impRegistry.resolve(inputPath, inputFmt);
        if (!imp) {
            std::println(stderr, "[error] {} cannot determine input format for '{}'",
                         nodehammer::codes::kErrImportFormatUnknown, inputPath);
            std::exit(1);
        }

        auto importResult = imp->import(inputPath);
        printDiags(importResult.diags);
        if (importResult.diags.hasErrors() ||
            (strict && !importResult.diags.empty() && !importResult.diags.hasErrors())) {
            std::println(stderr, "convert: import failed");
            std::exit(1);
        }

        // ── Select ─────────────────────────────────────────────────────────────
        if (!cfg.selection.empty()) {
            nodehammer::SelectionEngine sel{cfg.selection, cfg.hoistOrphans};
            auto selDiags = sel.prune(importResult.scene);
            printDiags(selDiags);
            if (selDiags.hasErrors()) {
                std::println(stderr, "convert: selection failed");
                std::exit(1);
            }
        }

        // ── Tessellate ─────────────────────────────────────────────────────────
        nodehammer::TessellationPass pass{cfg};
        auto tessResult = pass.lower(importResult.scene);
        printDiags(tessResult.diags);
        if (tessResult.diags.hasErrors()) {
            std::println(stderr, "convert: tessellation failed");
            std::exit(1);
        }

        // ── Export (one pass per output path) ─────────────────────────────────
        const auto expRegistry = nodehammer::makeDefaultExporterRegistry();

        // Look up per-format config; GLB falls back to "gltf" if "glb" isn't set.
        const auto applyFmtCfg = [&](nodehammer::ExportConfig &ecfg, const std::string &key) {
            if (!cfg.exportFormats.contains(key)) {
                return false;
            }
            const auto &variant = cfg.exportFormats.at(key);
            const auto &common = nodehammer::commonConfig(variant);
            if (auto v = common.unitScale) {
                ecfg.unitScale = *v;
            }
            if (auto v = common.bakeUnitScale) {
                ecfg.bakeUnitScale = *v;
            }
            if (const auto *gltfCfg = std::get_if<nodehammer::GltfExportFormatConfig>(&variant)) {
                if (auto v = gltfCfg->multiScene) {
                    ecfg.gltf.multiScene = *v;
                }
                if (auto v = gltfCfg->sceneNameSeparator) {
                    ecfg.gltf.sceneNameSeparator = *v;
                }
            }
            return true;
        };

        for (const auto &outputPath : outputPaths) {
            const auto *exp = expRegistry.resolve(outputPath, outputFmt);
            if (!exp) {
                std::println(stderr, "[error] {} cannot determine output format for '{}'",
                             nodehammer::codes::kErrExportWriteFailed, outputPath);
                std::exit(1);
            }

            nodehammer::ExportConfig ecfg;
            const std::string outExt = std::filesystem::path{outputPath}.extension().string();
            if (outputFmt == "glb" || outExt == ".glb") {
                ecfg.format = nodehammer::ExportConfig::Format::GLB;
            } else if (outputFmt == "gltf" || outExt == ".gltf") {
                ecfg.format = nodehammer::ExportConfig::Format::GLTF;
            } else if (outputFmt == "obj" || outExt == ".obj") {
                ecfg.format = nodehammer::ExportConfig::Format::OBJ;
            }

            ecfg.unitScale = nodehammer::ExportConfig::defaultUnitScale(ecfg.format);
            if (ecfg.format == nodehammer::ExportConfig::Format::OBJ) {
                ecfg.bakeUnitScale = true;
            }
            const std::string fmtKey =
                (ecfg.format == nodehammer::ExportConfig::Format::GLB)    ? "glb"
                : (ecfg.format == nodehammer::ExportConfig::Format::GLTF) ? "gltf"
                                                                          : "obj";
            if (!applyFmtCfg(ecfg, fmtKey) && fmtKey == "glb") {
                applyFmtCfg(ecfg, "gltf");
            }

            std::println("Writing {} ...", outputPath);
            auto expResult = exp->write(tessResult.scene, outputPath, ecfg);
            printDiags(expResult.diags);
            if (expResult.diags.hasErrors()) {
                std::println(stderr, "convert: export to '{}' failed", outputPath);
                std::exit(1);
            }

            int warnings = 0, errors = 0;
            for (const auto *dl : {&importResult.diags, &tessResult.diags, &expResult.diags}) {
                for (const auto &d : dl->items()) {
                    if (d.severity >= nodehammer::DiagnosticSeverity::Error) {
                        ++errors;
                    } else if (d.severity == nodehammer::DiagnosticSeverity::Warning) {
                        ++warnings;
                    }
                }
            }
            std::println("  Nodes: {}  Meshes: {}  Materials: {}  Warnings: {}  Errors: {}",
                         tessResult.scene.nodes.size(), tessResult.scene.meshAssets.size(),
                         tessResult.scene.materials.size(), warnings, errors);
        }
    });
}

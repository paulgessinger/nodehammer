#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <filesystem>
#include <format>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/detail/timing.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/render/exporter.hpp>
#include <nodehammer/ir/semantic/importer.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <print>
#include <string>

using nodehammer::cli::printDiags;

namespace {

[[nodiscard]] std::string formatHumanSize(std::uintmax_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    static constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(units)) {
        v /= 1024.0;
        ++u;
    }
    return std::format("{:.2f} {}", v, units[u]);
}

/// Paths that `convert` may produce in addition to the primary `-o` path.
[[nodiscard]] std::vector<std::filesystem::path>
convertOutputArtifacts(const std::filesystem::path &primary, nodehammer::ExportConfig::Format fmt) {
    std::vector<std::filesystem::path> paths;
    paths.push_back(primary);
    if (fmt == nodehammer::ExportConfig::Format::OBJ) {
        paths.emplace_back(primary.parent_path() / (primary.stem().string() + ".mtl"));
    } else if (fmt == nodehammer::ExportConfig::Format::GLTF) {
        paths.emplace_back(primary.parent_path() / (primary.stem().string() + ".bin"));
    }
    return paths;
}

void printWrittenOutputSizes(const std::vector<std::filesystem::path> &candidates) {
    std::string msg = "  Output:";
    bool any = false;
    for (const auto &p : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
            continue;
        }
        const auto sz = std::filesystem::file_size(p, ec);
        if (ec) {
            continue;
        }
        if (any) {
            msg += ", ";
        }
        msg += std::format(" {} ({})", p.string(), formatHumanSize(sz));
        any = true;
    }
    if (any) {
        std::println("{}", msg);
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
    auto *timingOpt = sub->add_flag("--timing", "Print per-step wall-clock timings");

    sub->callback([=] {
        std::string outputFmt;
        std::vector<std::string> outputPaths;
        outputOpt->results(outputPaths);
        if (*fmtOutOpt) {
            fmtOutOpt->results(outputFmt);
        }
        const bool strict = strictOpt->count() > 0;
        const bool showTiming = timingOpt->count() > 0;

        nodehammer::detail::TimingReport timings;

        // ── Load config ────────────────────────────────────────────────────────
        nodehammer::NHConfig cfg;
        if (*configOpt) {
            auto _t = timings.scope("config");
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
        nodehammer::detail::Timer importTimer;
        auto [importResult, importFmt] = nodehammer::cli::importOrExit(inputOpt, fmtInOpt);
        timings.record("import", importTimer.elapsed());
        printDiags(importResult.diags);
        if (importResult.diags.hasErrors() ||
            (strict && !importResult.diags.empty() && !importResult.diags.hasErrors())) {
            std::println(stderr, "convert: import failed");
            std::exit(1);
        }

        // ── Select ─────────────────────────────────────────────────────────────
        if (!cfg.selection.empty()) {
            auto _t = timings.scope("select");
            nodehammer::SelectionEngine sel{cfg.selection, cfg.hoistOrphans};
            auto selDiags = sel.prune(importResult.scene);
            printDiags(selDiags);
            if (selDiags.hasErrors()) {
                std::println(stderr, "convert: selection failed");
                std::exit(1);
            }
        }

        // ── Deduplicate shapes ─────────────────────────────────────────────────
        if (cfg.deduplicateShapes) {
            auto _t = timings.scope("deduplicate");
            const auto matsRemoved = importResult.scene.deduplicateMaterials();
            const auto shapesRemoved = importResult.scene.deduplicateShapes();
            const auto logVolsRemoved = importResult.scene.deduplicateLogVols();
            if (shapesRemoved > 0 || logVolsRemoved > 0 || matsRemoved > 0) {
                std::println(stderr,
                             "Dedup: {} shapes, {} logVols, {} materials merged ({} shapes, {} "
                             "logVols, {} materials unique)",
                             shapesRemoved, logVolsRemoved, matsRemoved,
                             importResult.scene.shapes.size(), importResult.scene.logVols.size(),
                             importResult.scene.materials.size());
            }
        }

        // ── Tessellate ─────────────────────────────────────────────────────────
        nodehammer::detail::Timer tessTimer;
        nodehammer::TessellationPass pass{cfg};
        auto tessResult = pass.lower(importResult.scene);
        timings.record("tessellate", tessTimer.elapsed());
        printDiags(tessResult.diags);
        if (tessResult.diags.hasErrors()) {
            std::println(stderr, "convert: tessellation failed");
            std::exit(1);
        }

        // ── Export (one pass per output path) ─────────────────────────────────
        const auto expRegistry = nodehammer::RenderExporterRegistry::makeDefault();

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
            nodehammer::detail::Timer expTimer;
            auto expResult = exp->write(tessResult.scene, outputPath, ecfg);
            timings.record(std::format("export[{}]", outputPath), expTimer.elapsed());
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
            std::size_t totalTris = 0;
            for (const auto &[id, ma] : tessResult.scene.meshAssets) {
                totalTris += ma.indices.size() / 3;
            }
            std::println("  Nodes: {}  Meshes: {}  Triangles: {}  Materials: {}  "
                         "Warnings: {}  Errors: {}",
                         tessResult.scene.nodes.size(), tessResult.scene.meshAssets.size(),
                         totalTris, tessResult.scene.materials.size(), warnings, errors);
            printWrittenOutputSizes(convertOutputArtifacts(outputPath, ecfg.format));
        }

        if (showTiming) {
            timings.print(stderr, "Timings");
        }
    });
}

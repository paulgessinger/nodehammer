#include "cli_common.hpp"
#include "run_internal.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <detail/timing.hpp>
#include <diagnostic_codes.hpp>
#include <export_resolve.hpp>
#include <filesystem>
#include <format>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/render/exporter.hpp>
#include <ir/semantic/exporter.hpp>
#include <ir/semantic/importer.hpp>
#include <ir/synthetic/semantic/importer.hpp>
#include <print>
#include <selection/selector.hpp>
#include <string>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>

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
convertOutputArtifacts(const std::filesystem::path &primary,
                       nodehammer::ir::ExportConfig::Format fmt) {
    std::vector<std::filesystem::path> paths;
    paths.push_back(primary);
    if (fmt == nodehammer::ir::ExportConfig::Format::OBJ) {
        paths.emplace_back(primary.parent_path() / (primary.stem().string() + ".mtl"));
    } else if (fmt == nodehammer::ir::ExportConfig::Format::GLTF) {
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

/// `--strict` as what it is: a policy the caller applies to a list it was
/// handed, not a flag threaded into the library (docs/error-model.md). Without
/// it, only an error is fatal to a conversion; with it, so is a warning.
struct Strictness {
    bool warningsAreFatal = false;

    void operator()(const nodehammer::DiagnosticList &diags, std::string_view stage) const {
        nodehammer::diagnostics::throwIfErrors(diags, stage);
        if (warningsAreFatal && !diags.empty()) {
            throw nodehammer::Error{nodehammer::codes::kErrConfigParse,
                                    "warnings are errors under --strict", stage, diags};
        }
    }
};

} // namespace

namespace nodehammer::cli::detail {

void registerCmdConvert(CLI::App &app, const RunOptions &) {
    auto *sub = app.add_subcommand(
        "convert", "Import geometry, apply a config, and write it out. The output format "
                   "decides how far the pipeline runs: .nhb stops at the semantic scene, "
                   ".glb/.obj/.nhr tessellate first.");

    // Not `->required()` any more: `--synthetic-box` supplies its own scene, and
    // a required option would make the flag unreachable. Checked below instead,
    // where the alternative can be named in the message.
    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *fmtInOpt =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *configOpt = sub->add_option("-c,--config", "Config file (.toml / .lua)");
    auto *outputOpt = sub->add_option("-o,--output", "Output file(s) -- one per format")
                          ->required()
                          ->expected(1, -1);
    auto *fmtOutOpt = sub->add_option(
        "--output-format",
        "Output format (auto-detected from extension if omitted; applies to all outputs). "
        "Semantic: json, nhb. Render: gltf, obj, nhr, render-json.");
    auto *strictOpt = sub->add_flag("--strict", "Treat warnings as errors");
    auto *timingOpt = sub->add_flag("--timing", "Print per-step wall-clock timings");
    auto *sizeReportOpt =
        sub->add_flag("--size-report", "Print estimated FlatBuffer payload breakdown to stderr");
    auto *syntheticBoxOpt =
        sub->add_flag("--synthetic-box", "Use a synthetic single-box scene instead of --input");

    // `--wedge-cut`, not `--angle-cut`: the viewer has an `--angle-cut` too and
    // it is a different operation -- a shader effect on the view, where this
    // rebuilds the geometry through a Manifold boolean. The help text already
    // called this one a wedge cut.
    auto *wedgeCutOpt =
        sub->add_option("--wedge-cut",
                        "Apply a precise azimuthal wedge cut (Manifold boolean): removes the "
                        "sector from START to END degrees (from +x, CCW)")
            ->expected(2)
            ->type_name("START END");
    auto *wcMarginOpt =
        sub->add_option("--wedge-cut-margin", "Cutting-solid oversize factor (default 2.0)")
            ->needs(wedgeCutOpt);

    sub->callback([=] {
        runOrReport("convert", [&] {
            std::string outputFmt;
            std::vector<std::string> outputPaths;
            outputOpt->results(outputPaths);
            if (*fmtOutOpt) {
                fmtOutOpt->results(outputFmt);
            }
            const Strictness demandClean{strictOpt->count() > 0};
            const bool showTiming = timingOpt->count() > 0;

            nodehammer::detail::TimingReport timings;

            // ── Decide how far to run ──────────────────────────────────────────────
            //
            // The outputs say it. A `.nhb` wants the semantic scene and no
            // tessellation; a `.glb` wants the mesh. Asking for both in one run
            // is not two runs -- the pipeline goes as deep as the deepest output
            // needs and writes each on the way past.
            //
            // The semantic registry is consulted first, which is what settles
            // `.json`: both scenes have a JSON form and both claim the
            // extension, so the bare spelling means the shallower one and the
            // render form is reached by naming it (`--output-format
            // render-json`).
            const auto semRegistry = nodehammer::ir::SemanticExporterRegistry::makeDefault();
            const auto renderRegistry = nodehammer::ir::RenderExporterRegistry::makeDefault();

            struct Target {
                std::string path;
                const nodehammer::ir::ISemanticExporter *semantic = nullptr;
                const nodehammer::ir::IRenderExporter *render = nullptr;
            };
            std::vector<Target> targets;
            targets.reserve(outputPaths.size());
            for (const auto &outputPath : outputPaths) {
                Target target{outputPath};
                target.semantic = semRegistry.resolve(outputPath, outputFmt);
                if (target.semantic == nullptr) {
                    target.render = renderRegistry.resolve(outputPath, outputFmt);
                }
                if (target.semantic == nullptr && target.render == nullptr) {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalExportWriteFailed,
                        std::format("cannot determine output format for '{}'", outputPath),
                        outputPath};
                }
                targets.push_back(target);
            }
            const bool needsTessellation =
                std::ranges::any_of(targets, [](const Target &t) { return t.render != nullptr; });

            // ── Load config ────────────────────────────────────────────────────────
            nodehammer::config::NHConfig cfg;
            if (*configOpt) {
                auto _t = timings.scope("config");
                std::string cfgPath;
                configOpt->results(cfgPath);
                auto loaded = nodehammer::config::ConfigLoader::loadFromFile(cfgPath);
                printDiags(loaded.diags);
                cfg = std::move(loaded.config);

                auto validDiags = nodehammer::config::ConfigValidator::validate(cfg);
                printDiags(validDiags);
                nodehammer::diagnostics::throwIfErrors(validDiags, cfgPath);
            }

            // ── Import ─────────────────────────────────────────────────────────────
            nodehammer::ir::ImportResult importResult;
            std::string importFmt;
            if (syntheticBoxOpt->count() > 0) {
                importResult.scene = nodehammer::ir::SyntheticSceneBuilder::buildSingleBox();
                importFmt = "synthetic";
            } else if (*inputOpt) {
                nodehammer::detail::Timer importTimer;
                auto imported = importFrom(inputOpt, fmtInOpt);
                timings.record("import", importTimer.elapsed());
                importResult = std::move(imported.result);
                importFmt = std::move(imported.formatName);
                printDiags(importResult.diags);
                demandClean(importResult.diags, "import");
            } else {
                throw nodehammer::Error{nodehammer::codes::kFatalCliUsage,
                                        "convert needs --input, or --synthetic-box"};
            }
            (void)importFmt;

            // ── Select ─────────────────────────────────────────────────────────────
            if (!cfg.selection.empty()) {
                auto _t = timings.scope("select");
                nodehammer::selection::SelectionEngine sel{cfg.selection, cfg.hoistOrphans};
                auto selDiags = sel.prune(importResult.scene);
                printDiags(selDiags);
                demandClean(selDiags, "selection");
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
                                 importResult.scene.shapes.size(),
                                 importResult.scene.logVols.size(),
                                 importResult.scene.materials.size());
                }
            }

            // ── Wedge cut (optional) ────────────────────────────────────────────────
            //
            // Before the semantic outputs are written, not after: the cut
            // rebuilds the geometry, so an archive or a blob written from this
            // run should carry it.
            if (wedgeCutOpt->count() > 0) {
                auto _t = timings.scope("wedgecut");
                std::vector<double> angles;
                wedgeCutOpt->results(angles); // exactly 2 (enforced by expected(2))
                nodehammer::tessellation::WedgeCutParams wcp;
                wcp.startDeg = angles[0];
                wcp.endDeg = angles[1];
                if (*wcMarginOpt) {
                    wcp.margin = wcMarginOpt->as<double>();
                }
                const auto wcStats =
                    nodehammer::tessellation::applyWedgeCut(importResult.scene, wcp);
                std::println(
                    stderr,
                    "Wedge cut [{:.1f}°,{:.1f}°]: {} cut ({} unique meshes), {} emptied, {} kept, "
                    "{} skipped, {} pruned",
                    wcp.startDeg, wcp.endDeg, wcStats.cut, wcStats.cutUnique, wcStats.emptied,
                    wcStats.kept, wcStats.skipped, wcStats.pruned);
            }

            if (sizeReportOpt->count() > 0) {
                const auto report =
                    nodehammer::ir::semanticFlatbufferSizeReport(importResult.scene);
                std::print(stderr, "{}",
                           nodehammer::ir::formatSemanticFlatbufferSizeReport(report));
            }

            // ── Write whatever stops here ──────────────────────────────────────────
            for (const auto &target : targets) {
                if (target.semantic == nullptr) {
                    continue;
                }
                std::println("Writing {} ...", target.path);
                nodehammer::detail::Timer expTimer;
                target.semantic->write(importResult.scene, target.path,
                                       nodehammer::ir::SemanticExportConfig{});
                timings.record(std::format("export[{}]", target.path), expTimer.elapsed());
                std::println("  Nodes: {}  Shapes: {}  Materials: {}",
                             importResult.scene.nodes.size(), importResult.scene.shapes.size(),
                             importResult.scene.materials.size());
                printWrittenOutputSizes({std::filesystem::path{target.path}});
            }

            if (!needsTessellation) {
                if (showTiming) {
                    timings.print(stderr, "Timings");
                }
                return;
            }

            // ── Tessellate ─────────────────────────────────────────────────────────
            nodehammer::detail::Timer tessTimer;
            nodehammer::tessellation::TessellationPass pass{cfg};
            auto tessResult = pass.lower(importResult.scene);
            timings.record("tessellate", tessTimer.elapsed());
            printDiags(tessResult.diags);
            // Tessellation errors are partial results — a node without a mesh — so the
            // library hands the scene back and lets the caller judge it. `convert`
            // judges that a scene missing geometry is not worth writing.
            demandClean(tessResult.diags, "tessellation");

            // ── Export (one pass per render output) ────────────────────────────────
            for (const auto &target : targets) {
                if (target.render == nullptr) {
                    continue;
                }
                const auto ecfg =
                    nodehammer::pipeline::resolveExportConfig(cfg, target.path, outputFmt);

                std::println("Writing {} ...", target.path);
                nodehammer::detail::Timer expTimer;
                target.render->write(tessResult.scene, target.path, ecfg);
                timings.record(std::format("export[{}]", target.path), expTimer.elapsed());

                int warnings = 0, errors = 0;
                for (const auto *dl : {&importResult.diags, &tessResult.diags}) {
                    for (const auto &d : dl->items()) {
                        if (d.severity >= nodehammer::diagnostics::Severity::Error) {
                            ++errors;
                        } else if (d.severity == nodehammer::diagnostics::Severity::Warning) {
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
                printWrittenOutputSizes(
                    convertOutputArtifacts(std::filesystem::path{target.path}, ecfg.format));
            }

            if (showTiming) {
                timings.print(stderr, "Timings");
            }
        });
    });
}

} // namespace nodehammer::cli::detail

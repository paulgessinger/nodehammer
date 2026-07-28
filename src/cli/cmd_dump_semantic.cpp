#include "cli_common.hpp"
#include "pager.hpp"

#include <CLI/CLI.hpp>
#include <cmath>
#include <nlohmann/json.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/detail/markup.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/fb/semantic/flatbuffer.hpp>
#include <nodehammer/ir/semantic/exporter.hpp>
#include <nodehammer/ir/semantic_json.hpp>
#include <nodehammer/scene.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/selection/selector.hpp>
#include <print>
#include <string>

namespace {

nodehammer::detail::ColorMode parseColorMode(const std::string &s) {
    if (s == "always") {
        return nodehammer::detail::ColorMode::Always;
    }
    if (s == "never") {
        return nodehammer::detail::ColorMode::Never;
    }
    return nodehammer::detail::ColorMode::Auto;
}

/// This command's own display names — "bool/union" rather than "union", and
/// the unknown type carried inline. ShapeKind is what makes that expressible
/// without visiting the variant.
std::string shapeTypeName(const nodehammer::ShapeView &shape) {
    switch (shape.kind()) {
    case nodehammer::ShapeKind::Union:
        return "bool/union";
    case nodehammer::ShapeKind::Intersection:
        return "bool/intersect";
    case nodehammer::ShapeKind::Subtraction:
        return "bool/subtract";
    case nodehammer::ShapeKind::Unknown:
        return std::format("unknown({})", shape.originalType());
    default:
        return std::string{shape.kindName()};
    }
}

std::string formatTranslation(const glm::dmat4 &m) {
    double x = m[3][0], y = m[3][1], z = m[3][2];
    if (std::abs(x) < 1e-6 && std::abs(y) < 1e-6 && std::abs(z) < 1e-6) {
        return "";
    }
    return std::format("{:.1f}, {:.1f}, {:.1f}", x, y, z);
}

void printRichTree(const nodehammer::SemanticScene &scene, int maxDepth, const std::string &filter,
                   const nodehammer::detail::Console &con) {
    int printed = 0;
    int filtered = 0;

    // One flag per depth is enough to rebuild the drawing prefix: preorder means
    // every ancestor was visited before us and its flag is still current.
    std::vector<bool> isLastAtDepth;

    scene.traverse([&](const nodehammer::SemanticScene::Visit &v) {
        const auto depth = static_cast<std::size_t>(v.depth);
        if (isLastAtDepth.size() <= depth) {
            isLastAtDepth.resize(depth + 1);
        }
        isLastAtDepth[depth] = v.isLastSibling;

        const bool show = filter.empty() || nodehammer::matchGlob(filter, v.node.originalPath());
        if (!show) {
            ++filtered;
            return maxDepth < 0 || v.depth < maxDepth;
        }

        std::string prefix;
        for (std::size_t d = 1; d < depth; ++d) {
            prefix += isLastAtDepth[d] ? "    " : "|   ";
        }

        std::string line;
        if (v.depth > 0) {
            line = std::format("[dim]{}[/]", v.isLastSibling ? "\\-- " : "|-- ");
        }
        line += std::format("[bold]{}[/]", v.node.name());

        std::string details;

        if (const auto shape = v.node.shape()) {
            details += std::format("[magenta]{}[/]", shapeTypeName(*shape));
        }
        if (const auto material = v.node.material()) {
            if (!details.empty()) {
                details += " ";
            }
            details += std::format("[green]{}[/]", material->name());
        }

        const auto trans = formatTranslation(v.node.localTransform());
        if (!trans.empty()) {
            if (!details.empty()) {
                details += " ";
            }
            details += std::format("[dim]@[/][blue]{}[/]", trans);
        }

        v.node.forEachTag([&](std::string_view k, std::string_view val) {
            if (!details.empty()) {
                details += " ";
            }
            details += std::format("[cyan]{}[/][dim]=[/][yellow]{}[/]", k, val);
        });

        const auto nChildren = v.node.childCount();
        if (nChildren > 0 && maxDepth >= 0 && v.depth == maxDepth) {
            if (!details.empty()) {
                details += " ";
            }
            details += std::format("[dim]{} children[/]", nChildren);
        } else if (v.node.isLeaf()) {
            if (!details.empty()) {
                details += " ";
            }
            details += "[dim]leaf[/]";
        }

        const std::string colorPrefix = std::format("[dim]{}[/]", prefix);
        if (!details.empty()) {
            con.println("{}{}  [dim]\\[[/]{}[dim]][/]", colorPrefix, line, details);
        } else {
            con.println("{}{}", colorPrefix, line);
        }
        ++printed;

        return maxDepth < 0 || v.depth < maxDepth;
    });

    if (!filter.empty()) {
        con.println("[dim]({} shown, {} filtered)[/]", printed, filtered);
    }
}

} // namespace

void registerCmdDumpSemantic(CLI::App &app) {
    auto *sub = app.add_subcommand("dump-semantic", "Dump the semantic IR of a geometry");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (required when --input is not given)");
    auto *configOpt = sub->add_option("-c,--config", "TOML config file (applies selection)");
    auto *outputOpt = sub->add_option("-o,--output", "Output file (default: stdout for json)");
    auto *richOpt = sub->add_flag("--rich", "Rich tree display instead of JSON");
    auto *depthOpt =
        sub->add_option("--depth,-d", "Maximum depth for --rich display (-1 = unlimited)")
            ->default_val(-1);
    auto *filterOpt = sub->add_option("--filter,-f", "Path glob filter for --rich display");
    auto *colorOpt = sub->add_option("--color", "Color output: auto, always, never")
                         ->default_val("auto")
                         ->check(CLI::IsMember({"auto", "always", "never"}));
    auto *outputFmtOpt = sub->add_option("--output-format", "Output format: json, nhb")
                             ->default_val("json")
                             ->check(CLI::IsMember({"json", "nhb"}));
    auto *sizeReportOpt =
        sub->add_flag("--size-report", "Print estimated FlatBuffer payload breakdown to stderr");

    sub->callback([=] {
        // ── Load config (optional) ─────────────────────────────────────────────
        nodehammer::NHConfig cfg;
        if (*configOpt) {
            std::string cfgPath;
            configOpt->results(cfgPath);
            auto loaded = nodehammer::ConfigLoader::loadFromFile(cfgPath);
            nodehammer::cli::printDiags(loaded.diags);
            if (loaded.diags.hasErrors()) {
                std::println(stderr, "dump-semantic: config load failed");
                return;
            }
            cfg = std::move(loaded.config);
            auto validDiags = nodehammer::ConfigValidator::validate(cfg);
            nodehammer::cli::printDiags(validDiags);
            if (validDiags.hasErrors()) {
                std::println(stderr, "dump-semantic: config validation failed");
                return;
            }
        }

        // ── Import ─────────────────────────────────────────────────────────────
        auto [result, fmt] = nodehammer::cli::importOrExit(inputOpt, formatOpt);
        nodehammer::cli::printDiags(result.diags);

        // ── Select ─────────────────────────────────────────────────────────────
        if (!cfg.selection.empty()) {
            nodehammer::SelectionEngine sel{cfg.selection, cfg.hoistOrphans};
            auto selDiags = sel.prune(result.scene);
            nodehammer::cli::printDiags(selDiags);
        }

        // ── Deduplicate shapes ─────────────────────────────────────────────────
        if (cfg.deduplicateShapes) {
            result.scene.deduplicateMaterials();
            result.scene.deduplicateShapes();
            result.scene.deduplicateLogVols();
        }

        if (sizeReportOpt->count() > 0) {
            auto report = nodehammer::semanticFlatbufferSizeReport(result.scene);
            std::print(stderr, "{}", nodehammer::formatSemanticFlatbufferSizeReport(report));
        }

        // ── Output ─────────────────────────────────────────────────────────────
        if (richOpt->count() > 0) {
            int maxDepth = -1;
            depthOpt->results(maxDepth);
            std::string filter;
            if (*filterOpt) {
                filterOpt->results(filter);
            }
            std::string colorStr;
            colorOpt->results(colorStr);

            nodehammer::cli::Pager pager;
            nodehammer::detail::Console con{pager.effectiveColorMode(parseColorMode(colorStr))};
            // Moved, not copied: this branch is exclusive with the serializing
            // one below, and a 325k-node scene is not worth duplicating.
            printRichTree(nodehammer::wrapSemanticScene(std::move(result.scene)), maxDepth, filter,
                          con);
        } else {
            std::string outPath;
            if (*outputOpt) {
                outputOpt->results(outPath);
            }

            // No output file: allow JSON to stream to stdout, but require -o for
            // binary formats (e.g. nhb) to avoid writing opaque bytes to terminal.
            if (outPath.empty()) {
                std::string outputFmt;
                outputFmtOpt->results(outputFmt);
                if (outputFmt != "json") {
                    std::println(stderr, "dump-semantic: {} output requires -o/--output",
                                 outputFmt);
                    return;
                }
                nlohmann::json j = result.scene;
                std::println("{}", j.dump(-1));
                return;
            }

            auto exporters = nodehammer::SemanticExporterRegistry::makeDefault();
            const nodehammer::ISemanticExporter *exporter = nullptr;

            // Preserve prior behavior:
            // 1) explicit --output-format wins
            // 2) otherwise infer from extension (including compound extensions)
            // 3) otherwise fallback to JSON
            if (*outputFmtOpt) {
                std::string outputFmt;
                outputFmtOpt->results(outputFmt);
                exporter = exporters.resolve(outPath, outputFmt);
            } else {
                exporter = exporters.resolve(outPath);
                if (exporter == nullptr) {
                    exporter = exporters.findByFormat("json");
                }
            }

            if (exporter == nullptr) {
                std::println(stderr, "[error] {} cannot determine output format for '{}'",
                             nodehammer::codes::kErrExportWriteFailed, outPath);
                return;
            }

            nodehammer::SemanticExportConfig exportCfg;
            auto exportResult = exporter->write(result.scene, outPath, exportCfg);
            nodehammer::cli::printDiags(exportResult.diags);
            if (exportResult.diags.hasErrors()) {
                std::println(stderr, "dump-semantic: export failed");
            }
        }
    });
}

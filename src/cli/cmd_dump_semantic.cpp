#include "cli_common.hpp"
#include "pager.hpp"

#include <CLI/CLI.hpp>
#include <cmath>
#include <nlohmann/json.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/fb/semantic/flatbuffer.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/ir/semantic/exporter.hpp>
#include <nodehammer/markup.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/selection/selector.hpp>
#include <print>
#include <string>

namespace {

nodehammer::ColorMode parseColorMode(const std::string &s) {
    if (s == "always") {
        return nodehammer::ColorMode::Always;
    }
    if (s == "never") {
        return nodehammer::ColorMode::Never;
    }
    return nodehammer::ColorMode::Auto;
}

std::string shapeTypeName(const nodehammer::SemanticShapeVariant &data) {
    return std::visit(
        nodehammer::detail::overloaded{
            [](const nodehammer::BoxShape &) -> std::string { return "box"; },
            [](const nodehammer::TubeShape &) -> std::string { return "tube"; },
            [](const nodehammer::ConeShape &) -> std::string { return "cone"; },
            [](const nodehammer::TrdShape &) -> std::string { return "trd"; },
            [](const nodehammer::ParaShape &) -> std::string { return "para"; },
            [](const nodehammer::PconShape &) -> std::string { return "pcon"; },
            [](const nodehammer::PgonShape &) -> std::string { return "pgon"; },
            [](const nodehammer::TorusShape &) -> std::string { return "torus"; },
            [](const nodehammer::TessellatedShape &) -> std::string { return "tessellated"; },
            [](const nodehammer::BooleanUnion &) -> std::string { return "bool/union"; },
            [](const nodehammer::BooleanIntersection &) -> std::string { return "bool/intersect"; },
            [](const nodehammer::BooleanSubtraction &) -> std::string { return "bool/subtract"; },
            [](const nodehammer::UnknownShape &s) -> std::string {
                return std::format("unknown({})", s.originalType);
            },
        },
        data);
}

std::string formatTranslation(const glm::dmat4 &m) {
    double x = m[3][0], y = m[3][1], z = m[3][2];
    if (std::abs(x) < 1e-6 && std::abs(y) < 1e-6 && std::abs(z) < 1e-6) {
        return "";
    }
    return std::format("{:.1f}, {:.1f}, {:.1f}", x, y, z);
}

void printRichTree(const nodehammer::SemanticScene &scene, int maxDepth, const std::string &filter,
                   const nodehammer::Console &con) {
    if (scene.nodes.empty() || !scene.nodes.contains(scene.rootId)) {
        return;
    }

    struct Entry {
        nodehammer::SemanticNodeId id;
        int depth;
        std::string prefix;
        bool isLast;
    };

    std::vector<Entry> stack;
    stack.push_back({scene.rootId, 0, "", true});

    int printed = 0;
    int filtered = 0;

    while (!stack.empty()) {
        auto [id, depth, prefix, isLast] = stack.back();
        stack.pop_back();

        if (!scene.nodes.contains(id)) {
            continue;
        }
        const auto &node = scene.nodes.at(id);

        if (maxDepth >= 0 && depth > maxDepth) {
            continue;
        }

        bool show = true;
        if (!filter.empty()) {
            show = nodehammer::matchGlob(filter, node.originalPath);
        }

        if (show) {
            // Tree connector
            std::string line;
            if (depth > 0) {
                line = std::format("[dim]{}[/]", isLast ? "\\-- " : "|-- ");
            }

            // Node name (bold)
            line += std::format("[bold]{}[/]", node.name);

            // Build detail annotations
            std::string details;

            // Shape + material from logical volume
            if (scene.logVols.contains(node.logVolId)) {
                const auto &lv = scene.logVols.at(node.logVolId);
                if (scene.shapes.contains(lv.shapeId)) {
                    const auto &shape = scene.shapes.at(lv.shapeId);
                    details += std::format("[magenta]{}[/]", shapeTypeName(shape.data));
                }
                if (scene.materials.contains(lv.materialId)) {
                    const auto &mat = scene.materials.at(lv.materialId);
                    if (!details.empty()) {
                        details += " ";
                    }
                    details += std::format("[green]{}[/]", mat.name);
                }
            }

            // Translation (if non-zero)
            auto trans = formatTranslation(node.localTransform);
            if (!trans.empty()) {
                if (!details.empty()) {
                    details += " ";
                }
                details += std::format("[dim]@[/][blue]{}[/]", trans);
            }

            // Tags
            if (!node.tags.empty()) {
                for (const auto &[k, v] : node.tags) {
                    if (!details.empty()) {
                        details += " ";
                    }
                    details += std::format("[cyan]{}[/][dim]=[/][yellow]{}[/]", k, v);
                }
            }

            // Child count at depth limit
            int nChildren = static_cast<int>(node.children.size());
            if (nChildren > 0 && maxDepth >= 0 && depth == maxDepth) {
                if (!details.empty()) {
                    details += " ";
                }
                details += std::format("[dim]{} children[/]", nChildren);
            } else if (node.children.empty()) {
                if (!details.empty()) {
                    details += " ";
                }
                details += "[dim]leaf[/]";
            }

            std::string colorPrefix = std::format("[dim]{}[/]", prefix);

            if (!details.empty()) {
                con.println("{}{}  [dim]\\[[/]{}[dim]][/]", colorPrefix, line, details);
            } else {
                con.println("{}{}", colorPrefix, line);
            }
            ++printed;
        } else {
            ++filtered;
        }

        const std::string childPrefix = (depth > 0) ? prefix + (isLast ? "    " : "|   ") : "";
        for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
            bool childIsLast = (i == static_cast<int>(node.children.size()) - 1);
            stack.push_back(
                {node.children[static_cast<std::size_t>(i)], depth + 1, childPrefix, childIsLast});
        }
    }

    if (!filter.empty()) {
        con.println("[dim]({} shown, {} filtered)[/]", printed, filtered);
    }
}

} // namespace

void register_cmd_dump_semantic(CLI::App &app) {
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
            nodehammer::Console con{pager.effectiveColorMode(parseColorMode(colorStr))};
            printRichTree(result.scene, maxDepth, filter, con);
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

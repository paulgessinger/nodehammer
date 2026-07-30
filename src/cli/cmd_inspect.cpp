#include "cli_common.hpp"
#include "pager.hpp"

#include <CLI/CLI.hpp>
#include <detail/markup.hpp>
#include <detail/overloaded.hpp>
#include <ir/semantic.hpp>
#include <map>
#include <print>
#include <selection/predicate.hpp>
#include <set>
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

// ── Summary ─��─────────────────────────────────���──────────────────────────────

void printSummary(const nodehammer::ir::ImportResult &result, std::string_view formatName) {
    std::map<std::string, int> shapeCounts;
    for (const auto &[id, shape] : result.scene.shapes) {
        std::string typeName = std::visit(
            nodehammer::detail::overloaded{
                [](const nodehammer::ir::semantic::BoxShape &) -> std::string { return "box"; },
                [](const nodehammer::ir::semantic::TubeShape &) -> std::string { return "tube"; },
                [](const nodehammer::ir::semantic::ConeShape &) -> std::string { return "cone"; },
                [](const nodehammer::ir::semantic::TrdShape &) -> std::string { return "trd"; },
                [](const nodehammer::ir::semantic::ParaShape &) -> std::string { return "para"; },
                [](const nodehammer::ir::semantic::PconShape &) -> std::string { return "pcon"; },
                [](const nodehammer::ir::semantic::PgonShape &) -> std::string { return "pgon"; },
                [](const nodehammer::ir::semantic::TorusShape &) -> std::string { return "torus"; },
                [](const nodehammer::ir::semantic::TessellatedShape &) -> std::string {
                    return "tessellated";
                },
                [](const nodehammer::ir::semantic::BooleanUnion &) -> std::string {
                    return "union";
                },
                [](const nodehammer::ir::semantic::BooleanIntersection &) -> std::string {
                    return "intersection";
                },
                [](const nodehammer::ir::semantic::BooleanSubtraction &) -> std::string {
                    return "subtraction";
                },
                [](const nodehammer::ir::semantic::UnknownShape &) -> std::string {
                    return "unknown";
                },
            },
            shape.data);
        shapeCounts[typeName]++;
    }

    std::vector<std::string> matNames;
    matNames.reserve(result.scene.materials.size());
    for (const auto &[id, mat] : result.scene.materials) {
        matNames.push_back(mat.name);
    }

    int warnings = 0, errors = 0;
    for (const auto &d : result.diags.items()) {
        if (d.severity >= nodehammer::diagnostics::DiagnosticSeverity::Error) {
            ++errors;
        } else if (d.severity == nodehammer::diagnostics::DiagnosticSeverity::Warning) {
            ++warnings;
        }
    }

    std::println("Format:    {}", formatName);
    std::println("Nodes:     {}", result.scene.nodes.size());

    std::string shapeStr;
    for (const auto &[name, count] : shapeCounts) {
        if (!shapeStr.empty()) {
            shapeStr += ", ";
        }
        shapeStr += std::format("{}={}", name, count);
    }
    std::println("Shapes:    {}", shapeStr.empty() ? "(none)" : shapeStr);

    std::string matStr;
    for (const auto &n : matNames) {
        if (!matStr.empty()) {
            matStr += ", ";
        }
        matStr += n;
    }
    std::println("Materials ({}): {}", matNames.size(), matStr.empty() ? "(none)" : matStr);
    std::println("Warnings: {}  Errors: {}", warnings, errors);
}

// ── Tree ───────────────────────────���───────────────────────────���─────────────

void printTree(const nodehammer::ir::semantic::Scene &scene, int maxDepth,
               const std::string &filter, const nodehammer::detail::Console &con) {
    if (scene.nodes.empty() || !scene.nodes.contains(scene.rootId)) {
        return;
    }

    struct Entry {
        nodehammer::ir::semantic::NodeId id;
        int depth;
        std::string prefix; // tree-drawing prefix
        bool isLast;
    };

    // DFS via explicit stack for tree drawing.
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

        // Check depth limit.
        if (maxDepth >= 0 && depth > maxDepth) {
            continue;
        }

        // Check filter.
        bool show = true;
        if (!filter.empty()) {
            show = nodehammer::selection::matchGlob(filter, node.originalPath);
        }

        if (show) {
            // Draw tree lines.
            std::string line;
            if (depth > 0) {
                line = std::format("[dim]{}[/]", isLast ? "\\-- " : "|-- ");
            }
            bool isLeaf = node.children.empty();
            line += std::format("[bold]{}[/]", node.name);

            // Annotations.
            std::string ann;
            if (!node.tags.empty()) {
                for (const auto &[k, v] : node.tags) {
                    if (!ann.empty()) {
                        ann += "[dim],[/] ";
                    }
                    ann += std::format("[cyan]{}[/][dim]=[/][yellow]{}[/]", k, v);
                }
            }
            int nChildren = static_cast<int>(node.children.size());
            if (nChildren > 0 && maxDepth >= 0 && depth == maxDepth) {
                if (!ann.empty()) {
                    ann += "[dim],[/] ";
                }
                ann += std::format("[dim]{} children[/]", nChildren);
            }
            if (isLeaf) {
                if (!ann.empty()) {
                    ann += "[dim],[/] ";
                }
                ann += "[dim]leaf[/]";
            }

            // Colorize the prefix (tree lines).
            std::string colorPrefix = std::format("[dim]{}[/]", prefix);

            if (!ann.empty()) {
                con.println("{}{}  [dim]\\[[/]{}[dim]][/]", colorPrefix, line, ann);
            } else {
                con.println("{}{}", colorPrefix, line);
            }
            ++printed;
        } else {
            ++filtered;
        }

        // Push children in reverse order so they come off the stack in order.
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

// ── Tags ──────────��───────────────────────────────���──────────────────────────

void printTags(const nodehammer::ir::semantic::Scene &scene,
               const nodehammer::detail::Console &con) {
    // Collect unique tag keys and their value sets.
    std::map<std::string, std::set<std::string>> tagValues;
    int nodesWithTags = 0;
    for (const auto &[id, node] : scene.nodes) {
        if (!node.tags.empty()) {
            ++nodesWithTags;
        }
        for (const auto &[k, v] : node.tags) {
            tagValues[k].insert(v);
        }
    }

    con.println("Nodes with tags: [bold]{}[/] / {}", nodesWithTags, scene.nodes.size());
    con.println("Unique tag keys: [bold]{}[/]", tagValues.size());
    con.println("");

    for (const auto &[key, values] : tagValues) {
        if (values.size() <= 10) {
            std::string valStr;
            for (const auto &v : values) {
                if (!valStr.empty()) {
                    valStr += "[dim],[/] ";
                }
                valStr += std::format("[yellow]\"{}\"[/]", v);
            }
            con.println("  [cyan]tag.{}[/] [dim]({} values):[/] {}", key, values.size(), valStr);
        } else {
            std::string sample;
            int n = 0;
            for (const auto &v : values) {
                if (n >= 5) {
                    break;
                }
                if (!sample.empty()) {
                    sample += "[dim],[/] ";
                }
                sample += std::format("[yellow]\"{}\"[/]", v);
                ++n;
            }
            con.println("  [cyan]tag.{}[/] [dim]({} values):[/] {}[dim], ...[/]", key,
                        values.size(), sample);
        }
    }
}

} // namespace

void registerCmdInspect(CLI::App &app) {
    auto *sub = app.add_subcommand("inspect", "Inspect a geometry file")->require_subcommand(1);

    // Shared options on the parent.
    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *colorOpt = sub->add_option("--color", "Color output: auto, always, never")
                         ->default_val("auto")
                         ->check(CLI::IsMember({"auto", "always", "never"}));

    // ── summary ──────────────────────────────────────────────────────────────
    auto *sumSub = sub->add_subcommand("summary", "Print a high-level summary");
    sumSub->callback([=] {
        auto [result, fmt] = nodehammer::cli::importOrExit(inputOpt, formatOpt);
        nodehammer::cli::Pager pager;
        printSummary(result, fmt);
    });

    // ── tree ───��───────────────���─────────────────────────────────────────────
    auto *treeSub = sub->add_subcommand("tree", "Print the node hierarchy");
    auto *depthOpt = treeSub->add_option("--depth,-d", "Maximum depth to display (-1 = unlimited)")
                         ->default_val(-1);
    auto *filterOpt =
        treeSub->add_option("--filter,-f", "Path glob filter (only show matching nodes)");

    treeSub->callback([=] {
        auto [result, fmt] = nodehammer::cli::importOrExit(inputOpt, formatOpt);

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
        printTree(result.scene, maxDepth, filter, con);
    });

    // ── tags ────────────────────────────────────���───────────────────────────���
    auto *tagsSub = sub->add_subcommand("tags", "List all unique tags and their values");
    tagsSub->callback([=] {
        auto [result, fmt] = nodehammer::cli::importOrExit(inputOpt, formatOpt);

        std::string colorStr;
        colorOpt->results(colorStr);

        nodehammer::cli::Pager pager;
        nodehammer::detail::Console con{pager.effectiveColorMode(parseColorMode(colorStr))};
        printTags(result.scene, con);
    });
}

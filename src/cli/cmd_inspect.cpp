#include "cli_common.hpp"
#include "pager.hpp"

#include <CLI/CLI.hpp>
#include <map>
#include <nodehammer/detail/handle_seam.hpp>
#include <nodehammer/detail/markup.hpp>
#include <nodehammer/detail/scene_access.hpp>
#include <nodehammer/scene.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <print>
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

void printSummary(const nodehammer::SemanticScene &scene, const nodehammer::DiagnosticList &diags,
                  std::string_view formatName) {
    const auto shapeCounts = scene.shapeKindCounts();
    const auto matNames = scene.materialNames();

    int warnings = 0, errors = 0;
    for (const auto &d : diags.items()) {
        if (d.severity >= nodehammer::DiagnosticSeverity::Error) {
            ++errors;
        } else if (d.severity == nodehammer::DiagnosticSeverity::Warning) {
            ++warnings;
        }
    }

    std::println("Format:    {}", formatName);
    std::println("Nodes:     {}", scene.nodeCount());

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

void printTree(const nodehammer::SemanticScene &scene, int maxDepth, const std::string &filter,
               const nodehammer::detail::Console &con) {
    int printed = 0;
    int filtered = 0;

    // The drawing prefix is the ancestors' "am I the last child" flags. Preorder
    // guarantees every ancestor was visited before us and its flag is still
    // current, so tracking one flag per depth is enough — no need to carry a
    // built-up string through the traversal.
    std::vector<bool> isLastAtDepth;

    nodehammer::detail::traverse(scene, [&](const nodehammer::detail::Visit &v) {
        const auto depth = static_cast<std::size_t>(v.depth);
        if (isLastAtDepth.size() <= depth) {
            isLastAtDepth.resize(depth + 1);
        }
        isLastAtDepth[depth] = v.isLastSibling;

        const bool show = filter.empty() || nodehammer::matchGlob(filter, v.node.originalPath());
        if (!show) {
            ++filtered;
            // Filtering hides a node but not its descendants, as before.
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

        std::string ann;
        v.node.forEachTag([&](std::string_view k, std::string_view val) {
            if (!ann.empty()) {
                ann += "[dim],[/] ";
            }
            ann += std::format("[cyan]{}[/][dim]=[/][yellow]{}[/]", k, val);
        });
        const auto nChildren = v.node.childCount();
        if (nChildren > 0 && maxDepth >= 0 && v.depth == maxDepth) {
            if (!ann.empty()) {
                ann += "[dim],[/] ";
            }
            ann += std::format("[dim]{} children[/]", nChildren);
        }
        if (v.node.isLeaf()) {
            if (!ann.empty()) {
                ann += "[dim],[/] ";
            }
            ann += "[dim]leaf[/]";
        }

        const std::string colorPrefix = std::format("[dim]{}[/]", prefix);
        if (!ann.empty()) {
            con.println("{}{}  [dim]\\[[/]{}[dim]][/]", colorPrefix, line, ann);
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

// ── Tags ──────────��───────────────────────────────���──────────────────────────

void printTags(const nodehammer::SemanticScene &scene, const nodehammer::detail::Console &con) {
    // Collect unique tag keys and their value sets.
    std::map<std::string, std::set<std::string>> tagValues;
    int nodesWithTags = 0;
    for (const auto id : nodehammer::detail::nodeIds(scene)) {
        const auto node = *nodehammer::detail::node(scene, id);
        if (node.tagCount() > 0) {
            ++nodesWithTags;
        }
        node.forEachTag(
            [&](std::string_view k, std::string_view v) { tagValues[std::string{k}].emplace(v); });
    }

    con.println("Nodes with tags: [bold]{}[/] / {}", nodesWithTags, scene.nodeCount());
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
        printSummary(nodehammer::detail::wrapSemanticScene(std::move(result.scene)), result.diags,
                     fmt);
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
        printTree(nodehammer::detail::wrapSemanticScene(std::move(result.scene)), maxDepth, filter,
                  con);
    });

    // ── tags ────────────────────────────────────���───────────────────────────���
    auto *tagsSub = sub->add_subcommand("tags", "List all unique tags and their values");
    tagsSub->callback([=] {
        auto [result, fmt] = nodehammer::cli::importOrExit(inputOpt, formatOpt);

        std::string colorStr;
        colorOpt->results(colorStr);

        nodehammer::cli::Pager pager;
        nodehammer::detail::Console con{pager.effectiveColorMode(parseColorMode(colorStr))};
        printTags(nodehammer::detail::wrapSemanticScene(std::move(result.scene)), con);
    });
}

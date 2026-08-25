#include "cli_common.hpp"
#include "pager.hpp"
#include "run_internal.hpp"

#include <CLI/CLI.hpp>
#include <detail/markup.hpp>
#include <detail/overloaded.hpp>
#include <ir/semantic.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <print>
#include <selection/predicate.hpp>
#include <set>
#include <string>

namespace {

/// What `--format` selects.
///
/// Text and JSON are not the same content in two skins. The text renderer is a
/// *view*: it draws tree lines, elides a tag key with more than ten values down
/// to five, and appends a "(n shown, m filtered)" footer. Every one of those is
/// right for a person reading a terminal and wrong for a program parsing the
/// output, so the JSON side carries the whole set and no drawing at all.
enum class OutputFormat { Text, Json };

/// The version of the JSON documents below.
///
/// They are API from the moment they ship — a caller pattern-matches on the
/// field names — so the shape is stamped rather than left to be inferred. Same
/// role `schema` plays in `nh_runtime.json`.
constexpr int kJsonSchema = 1;

nodehammer::detail::ColorMode parseColorMode(const std::string &s) {
    if (s == "always") {
        return nodehammer::detail::ColorMode::Always;
    }
    if (s == "never") {
        return nodehammer::detail::ColorMode::Never;
    }
    return nodehammer::detail::ColorMode::Auto;
}

std::string shapeTypeName(const nodehammer::ir::semantic::Shape &shape) {
    return std::visit(
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
            [](const nodehammer::ir::semantic::BooleanUnion &) -> std::string { return "union"; },
            [](const nodehammer::ir::semantic::BooleanIntersection &) -> std::string {
                return "intersection";
            },
            [](const nodehammer::ir::semantic::BooleanSubtraction &) -> std::string {
                return "subtraction";
            },
            [](const nodehammer::ir::semantic::UnknownShape &) -> std::string { return "unknown"; },
        },
        shape.data);
}

/// Shape-type histogram, keyed by the same names the text summary prints.
std::map<std::string, int> shapeHistogram(const nodehammer::ir::semantic::Scene &scene) {
    std::map<std::string, int> counts;
    for (const auto &[id, shape] : scene.shapes) {
        counts[shapeTypeName(shape)]++;
    }
    return counts;
}

/// Warning and error totals over an import's diagnostics.
std::pair<int, int> diagnosticCounts(const nodehammer::DiagnosticList &diags) {
    int warnings = 0, errors = 0;
    for (const auto &d : diags.items()) {
        if (d.severity >= nodehammer::diagnostics::Severity::Error) {
            ++errors;
        } else if (d.severity == nodehammer::diagnostics::Severity::Warning) {
            ++warnings;
        }
    }
    return {warnings, errors};
}

// ── Summary ─��─────────────────────────────────���──────────────────────────────

void printSummary(const nodehammer::ir::ImportResult &result, std::string_view formatName) {
    const auto shapeCounts = shapeHistogram(result.scene);

    std::vector<std::string> matNames;
    matNames.reserve(result.scene.materials.size());
    for (const auto &[id, mat] : result.scene.materials) {
        matNames.push_back(mat.name);
    }

    const auto [warnings, errors] = diagnosticCounts(result.diags);

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

nlohmann::json summaryJson(const nodehammer::ir::ImportResult &result,
                           std::string_view formatName) {
    const auto [warnings, errors] = diagnosticCounts(result.diags);

    std::vector<std::string> matNames;
    matNames.reserve(result.scene.materials.size());
    for (const auto &[id, mat] : result.scene.materials) {
        matNames.push_back(mat.name);
    }
    // Sorted, unlike the text rendering, which reports them in map order. A
    // caller diffing two summaries wants the difference to mean something.
    std::ranges::sort(matNames);

    return nlohmann::json{
        {"schema", kJsonSchema},
        {"kind", "summary"},
        {"format", formatName},
        {"nodes", result.scene.nodes.size()},
        {"shapes", shapeHistogram(result.scene)},
        {"materials", matNames},
        {"diagnostics", {{"warnings", warnings}, {"errors", errors}}},
    };
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

/// The same walk the text renderer does, emitted as a flat list.
///
/// Flat, with `path` and `depth`, rather than nested children. A caller filters
/// and greps this; nesting would make every such question a recursive descent,
/// and the tree lines the text view draws are the only thing that needed the
/// shape in the first place. The parent is recoverable from the path.
nlohmann::json treeJson(const nodehammer::ir::semantic::Scene &scene, int maxDepth,
                        const std::string &filter) {
    nlohmann::json nodes = nlohmann::json::array();
    int shown = 0;
    int filtered = 0;

    struct Entry {
        nodehammer::ir::semantic::NodeId id;
        int depth;
    };
    std::vector<Entry> stack;
    if (!scene.nodes.empty() && scene.nodes.contains(scene.rootId)) {
        stack.push_back({scene.rootId, 0});
    }

    while (!stack.empty()) {
        const auto [id, depth] = stack.back();
        stack.pop_back();
        if (!scene.nodes.contains(id)) {
            continue;
        }
        const auto &node = scene.nodes.at(id);
        if (maxDepth >= 0 && depth > maxDepth) {
            continue;
        }

        if (filter.empty() || nodehammer::selection::matchGlob(filter, node.originalPath)) {
            nlohmann::json entry{
                {"path", node.originalPath},
                {"name", node.name},
                {"depth", depth},
                {"children", node.children.size()},
                {"leaf", node.children.empty()},
            };
            if (!node.tags.empty()) {
                entry["tags"] = node.tags;
            }
            nodes.push_back(std::move(entry));
            ++shown;
        } else {
            ++filtered;
        }

        for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
            stack.push_back({node.children[static_cast<std::size_t>(i)], depth + 1});
        }
    }

    nlohmann::json doc{{"schema", kJsonSchema},
                       {"kind", "tree"},
                       {"shown", shown},
                       {"filtered", filtered},
                       {"nodes", std::move(nodes)}};
    // Present-and-null rather than absent, so a caller can read the field
    // unconditionally and learn that no limit applied.
    doc["maxDepth"] = maxDepth >= 0 ? nlohmann::json(maxDepth) : nlohmann::json();
    doc["filter"] = filter.empty() ? nlohmann::json() : nlohmann::json(filter);
    return doc;
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

nlohmann::json tagsJson(const nodehammer::ir::semantic::Scene &scene) {
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

    // Every value, however many there are. The text view samples five once a
    // key passes ten, which is a kindness to a reader and a lie to a parser --
    // and "what values does this key take" is most of why a caller asks.
    nlohmann::json keys = nlohmann::json::object();
    for (const auto &[key, values] : tagValues) {
        keys[key] = values;
    }

    return nlohmann::json{
        {"schema", kJsonSchema},           {"kind", "tags"},
        {"nodeCount", scene.nodes.size()}, {"nodesWithTags", nodesWithTags},
        {"keys", std::move(keys)},
    };
}

/// Print a JSON document, two-space indented and newline-terminated.
///
/// Never paged: a pager owns stdout until somebody quits it, which for a caller
/// reading the output is a hang rather than a convenience.
void emitJson(const nlohmann::json &doc) { std::println("{}", doc.dump(2)); }

} // namespace

namespace nodehammer::cli::detail {

void registerCmdInspect(CLI::App &app, const RunOptions &options) {
    auto *sub = app.add_subcommand("inspect", "Inspect a geometry file")->require_subcommand(1);

    // Shared options on the parent.
    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *colorOpt = sub->add_option("--color", "Color output: auto, always, never")
                         ->default_val("auto")
                         ->check(CLI::IsMember({"auto", "always", "never"}));
    // Default text, and *not* auto-detected from the TTY the way --color is.
    // Colour and paging are presentation, so flipping them on a pipe is safe;
    // flipping the document's structure is not -- a pipe added later would
    // silently break whatever was parsing the output.
    auto *formatOutOpt = sub->add_option("--output-format", "Report format: text, json")
                             ->default_val("text")
                             ->check(CLI::IsMember({"text", "json"}));

    // ── summary ──────────────────────────────────────────────────────────────
    auto *sumSub = sub->add_subcommand("summary", "Print a high-level summary");
    sumSub->callback([=, &options] {
        runOrReport("inspect summary", [&] {
            auto [result, fmt] = importFrom(inputOpt, formatOpt);
            if (formatOutOpt->as<std::string>() == "json") {
                emitJson(summaryJson(result, fmt));
                return;
            }
            Pager pager{options.pager};
            printSummary(result, fmt);
        });
    });

    // ── tree ───��───────────────���─────────────────────────────────────────────
    auto *treeSub = sub->add_subcommand("tree", "Print the node hierarchy");
    auto *depthOpt = treeSub->add_option("--depth,-d", "Maximum depth to display (-1 = unlimited)")
                         ->default_val(-1);
    auto *filterOpt =
        treeSub->add_option("--filter,-f", "Path glob filter (only show matching nodes)");

    treeSub->callback([=, &options] {
        runOrReport("inspect tree", [&] {
            auto [result, fmt] = importFrom(inputOpt, formatOpt);
            (void)fmt;

            int maxDepth = -1;
            depthOpt->results(maxDepth);
            std::string filter;
            if (*filterOpt) {
                filterOpt->results(filter);
            }

            if (formatOutOpt->as<std::string>() == "json") {
                emitJson(treeJson(result.scene, maxDepth, filter));
                return;
            }

            std::string colorStr;
            colorOpt->results(colorStr);

            Pager pager{options.pager};
            nodehammer::detail::Console con{pager.effectiveColorMode(parseColorMode(colorStr))};
            printTree(result.scene, maxDepth, filter, con);
        });
    });

    // ── tags ────────────────────────────────────���───────────────────────────���
    auto *tagsSub = sub->add_subcommand("tags", "List all unique tags and their values");
    tagsSub->callback([=, &options] {
        runOrReport("inspect tags", [&] {
            auto [result, fmt] = importFrom(inputOpt, formatOpt);
            (void)fmt;

            if (formatOutOpt->as<std::string>() == "json") {
                emitJson(tagsJson(result.scene));
                return;
            }

            std::string colorStr;
            colorOpt->results(colorStr);

            Pager pager{options.pager};
            nodehammer::detail::Console con{pager.effectiveColorMode(parseColorMode(colorStr))};
            printTags(result.scene, con);
        });
    });
}

} // namespace nodehammer::cli::detail

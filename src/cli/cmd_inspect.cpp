#include "cli_common.hpp"
#include "pager.hpp"
#include "run_internal.hpp"
#include <ir/expanded/conversion.hpp>

#include <CLI/CLI.hpp>
#include <detail/markup.hpp>
#include <detail/overloaded.hpp>
#include <ir/expanded/scene.hpp>
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
    std::println("Nodes:     {}", result.scene.nodeCount());

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
        {"nodes", result.scene.nodeCount()},
        {"shapes", shapeHistogram(result.scene)},
        {"materials", matNames},
        {"diagnostics", {{"warnings", warnings}, {"errors", errors}}},
    };
}

// ── Tree ───────────────────────────���───────────────────────────���─────────────

void printTree(const nodehammer::ir::semantic::Scene &scene, int maxDepth,
               const std::string &filter, const nodehammer::detail::Console &con) {
    uint64_t shown = 0, filtered = 0;
    std::vector<bool> lastAtDepth;
    nodehammer::ir::semantic::visit(scene, [&](const auto &node, bool last) {
        const auto depth = node.id.size();
        if (maxDepth >= 0 && depth > static_cast<std::size_t>(maxDepth))
            return false;
        lastAtDepth.resize(depth + 1);
        lastAtDepth[depth] = last;
        if (filter.empty() || nodehammer::selection::matchGlob(filter, node.originalPath)) {
            std::string prefix;
            for (std::size_t i = 1; i < depth; ++i)
                prefix += lastAtDepth[i] ? "    " : "|   ";
            if (depth)
                prefix += last ? "\\-- " : "|-- ";
            std::string annotations;
            for (const auto &[k, v] : node.tags) {
                if (!annotations.empty())
                    annotations += ", ";
                annotations += k + "=" + v;
            }
            if (!node.childCount) {
                if (!annotations.empty())
                    annotations += ", ";
                annotations += "leaf";
            } else if (maxDepth >= 0 && depth == static_cast<std::size_t>(maxDepth)) {
                if (!annotations.empty())
                    annotations += ", ";
                annotations += std::format("{} children", node.childCount);
            }
            con.println("{}{}{}", prefix, node.name,
                        annotations.empty() ? "" : "  \\[" + annotations + "]");
            ++shown;
        } else
            ++filtered;
        return maxDepth < 0 || depth < static_cast<std::size_t>(maxDepth);
    });
    if (!filter.empty())
        con.println("({} shown, {} filtered)", shown, filtered);
    return;
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
    uint64_t shown = 0;
    uint64_t filtered = 0;

    nodehammer::ir::semantic::visit(scene, [&](const auto &node, bool) {
        const auto depth = node.id.size();
        if (maxDepth >= 0 && depth > static_cast<std::size_t>(maxDepth))
            return false;
        if (filter.empty() || nodehammer::selection::matchGlob(filter, node.originalPath)) {
            nlohmann::json entry{{"path", node.originalPath},
                                 {"name", node.name},
                                 {"depth", depth},
                                 {"children", node.childCount},
                                 {"leaf", node.childCount == 0}};
            if (!node.tags.empty())
                entry["tags"] = node.tags;
            nodes.push_back(std::move(entry));
            ++shown;
        } else
            ++filtered;
        return maxDepth < 0 || depth < static_cast<std::size_t>(maxDepth);
    });
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
    uint64_t nodesWithTags = 0;

    nodehammer::ir::semantic::visit(scene, [&](const auto &node, bool) {
        if (!node.tags.empty())
            ++nodesWithTags;
        for (const auto &[k, v] : node.tags)
            tagValues[k].insert(v);
        return true;
    });
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

nlohmann::json tagsJson(const nodehammer::ir::semantic::Scene &scene) {
    std::map<std::string, std::set<std::string>> tagValues;
    uint64_t nodesWithTags = 0;

    nodehammer::ir::semantic::visit(scene, [&](const auto &node, bool) {
        if (!node.tags.empty())
            ++nodesWithTags;
        for (const auto &[k, v] : node.tags)
            tagValues[k].insert(v);
        return true;
    });
    // Every value, however many there are. The text view samples five once a
    // key passes ten, which is a kindness to a reader and a lie to a parser --
    // and "what values does this key take" is most of why a caller asks.
    nlohmann::json keys = nlohmann::json::object();
    for (const auto &[key, values] : tagValues) {
        keys[key] = values;
    }

    return nlohmann::json{
        {"schema", kJsonSchema},          {"kind", "tags"},
        {"nodeCount", scene.nodeCount()}, {"nodesWithTags", nodesWithTags},
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

void registerCmdInspect(CLI::App &app, const CliOptions &options) {
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
            printDiags(result.diags);
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
            printDiags(result.diags);
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
            printDiags(result.diags);
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

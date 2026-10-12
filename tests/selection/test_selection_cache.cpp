#include <catch2/catch_test_macros.hpp>
#include <config/predicate_parser.hpp>
#include <ir/expanded/conversion.hpp>
#include <selection/occurrence_selector.hpp>

#include <random>

using namespace nodehammer;
using namespace nodehammer::ir::semantic;
using namespace nodehammer::config;
using namespace nodehammer::selection;

namespace {
Scene cacheFixture(std::size_t towers = 12) {
    Scene scene;
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{1, 1, 1}});
    scene.materials.emplace(MaterialId{1}, SourceMaterial{MaterialId{1}, "Brass", {}, 1});
    scene.materials.emplace(MaterialId{2}, SourceMaterial{MaterialId{2}, "Optical", {}, 1});
    for (uint64_t i = 1; i <= 5; ++i)
        scene.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "volume", ShapeId{1},
                                                         MaterialId{i == 4 ? 2ULL : 1ULL}});
    scene.root = {"world", LogVolId{1}, glm::dmat4{1}};
    for (std::size_t i = 0; i < towers; ++i)
        scene.logVols.at(LogVolId{1})
            .daughters.push_back({"tower" + std::to_string(i), LogVolId{2}, glm::dmat4{1}});
    scene.logVols.at(LogVolId{2}).daughters = {{"capillary_1", LogVolId{3}, glm::dmat4{1}},
                                               {"capillary_2", LogVolId{3}, glm::dmat4{1}},
                                               {"support", LogVolId{5}, glm::dmat4{1}}};
    scene.logVols.at(LogVolId{3}).daughters = {{"core", LogVolId{4}, glm::dmat4{1}},
                                               {"clad", LogVolId{4}, glm::dmat4{1}}};
    scene.metadata.tagSets = {{{"sensitive", "true"}}, {{"sensitive", "false"}}, {}};
    scene.metadata.volumeTags[LogVolId{4}] = 0;
    scene.metadata.placementTags[{LogVolId{3}, 1}] = 1;
    return scene;
}
SelectionRule rule(SelectionAction action, std::string_view expression,
                   std::optional<std::string> scope = {}) {
    auto parsed = parsePredicateExpr(expression);
    REQUIRE(parsed.has_value());
    return SelectionRule{action, std::move(scope), std::move(*parsed)};
}
void sameDiagnostics(const DiagnosticList &a, const DiagnosticList &b) {
    REQUIRE(a.items().size() == b.items().size());
    for (std::size_t i = 0; i < a.items().size(); ++i) {
        REQUIRE(a.items()[i].code == b.items()[i].code);
        REQUIRE(a.items()[i].severity == b.items()[i].severity);
        REQUIRE(a.items()[i].message == b.items()[i].message);
        REQUIRE(a.items()[i].context == b.items()[i].context);
    }
}
void differential(const Scene &scene, const std::vector<std::vector<SelectionRule>> &passes,
                  bool hoist, std::optional<uint64_t> warningLimit = 3) {
    auto fastTree = scene.makeTree(0), referenceTree = scene.makeTree(0);
    OccurrenceSelectionOptions fastOptions, referenceOptions;
    fastOptions.orphanDiagnosticLimit = referenceOptions.orphanDiagnosticLimit = warningLimit;
    fastOptions.optimizeSharedEvaluation = true;
    referenceOptions.optimizeSharedEvaluation = false;
    SelectedOccurrences fast{*fastTree, fastOptions}, reference{*referenceTree, referenceOptions};
    for (const auto &rules : passes) {
        auto a = fast.dryRun(rules, hoist), b = reference.dryRun(rules, hoist);
        REQUIRE(a.keptCount() == b.keptCount());
        REQUIRE(a.droppedCount() == b.droppedCount());
        REQUIRE(a.orphanWarningCount() == b.orphanWarningCount());
        REQUIRE(a.omittedOrphanDiagnosticCount() == b.omittedOrphanDiagnosticCount());
        sameDiagnostics(a.diagnostics(), b.diagnostics());
        nodehammer::ir::semantic::visit(scene, [&](const auto &node, bool) {
            REQUIRE(a.contains(node.id) == b.contains(node.id));
            return true;
        });
        fast = fast.prune(rules, hoist);
        reference = reference.prune(rules, hoist);
        REQUIRE(fast.size() == reference.size());
    }
}
} // namespace

TEST_CASE("Shared selection optimization preserves scopes overrides and later keep rules",
          "[selection][selection-cache]") {
    auto scene = cacheFixture();
    scene.metadata.displayNames[{1, 0}] = "renamedSupport";
    scene.metadata.occurrenceTags[{2, 0, 0}] = 2;
    scene.metadata.pathNames[{3, 0}] = "a/path/with/slashes";
    scene.metadata.originalPaths[{4, 0, 0}] = "/independent/core";
    scene.metadata.displayNames[{5}] = "towerRenamed";
    const auto base = std::vector{
        rule(SelectionAction::DropIf, "name ~= \"capillary*\" || path ~= \"**/capillary*/**\""),
        rule(SelectionAction::DropIf, "material == \"Optical\"")};
    for (bool hoist : {false, true}) {
        DYNAMIC_SECTION("hoist = " << hoist) {
            SECTION("drop only") { differential(scene, {base}, hoist); }
            SECTION("later keep under a dropped ancestor") {
                auto rules = base;
                rules.push_back(
                    rule(SelectionAction::KeepIf, "tag.sensitive == \"true\"", "/world/tower0/**"));
                differential(scene, {rules}, hoist);
            }
            SECTION("original path escapes source subtree") {
                auto rules = base;
                rules.push_back(rule(SelectionAction::KeepIf, "path == \"/independent/core\""));
                differential(scene, {rules}, hoist);
            }
            SECTION("negative path and compound boolean") {
                differential(
                    scene,
                    {{rule(SelectionAction::DropIf, "!(path ~= \"**/capillary*/**\") && (name ~= "
                                                    "\"support\" || tag.sensitive == \"false\")")}},
                    hoist);
            }
        }
    }
}

TEST_CASE("Shared selection optimization preserves exact bounded orphan diagnostics",
          "[selection][selection-cache]") {
    const auto scene = cacheFixture();
    const std::vector rules{rule(SelectionAction::DropIf, "name ~= \"capillary*\"")};
    for (auto limit : {std::optional<uint64_t>{0}, std::optional<uint64_t>{1},
                       std::optional<uint64_t>{7}, std::optional<uint64_t>{}})
        differential(scene, {rules}, false, limit);
}

TEST_CASE("Shared selection optimization handles successive leaf and tag filters",
          "[selection][selection-cache]") {
    auto scene = cacheFixture();
    scene.metadata.occurrenceTags[{1, 0, 0}] = 2;
    const std::vector<std::vector<SelectionRule>> passes{
        {rule(SelectionAction::DropIf, "tag.sensitive == \"true\"")},
        {rule(SelectionAction::DropIf, "is_leaf && name ~= \"clad\"")},
        {rule(SelectionAction::DropIf, "is_leaf && name ~= \"capillary*\"")},
        {rule(SelectionAction::KeepIf, "true")}};
    differential(scene, passes, false);
    differential(scene, passes, true);
}

TEST_CASE("Shared selection optimization agrees with reference for generated rule sequences",
          "[selection][selection-cache]") {
    auto scene = cacheFixture(8);
    scene.metadata.occurrenceTags[{2, 1, 0}] = 2;
    scene.metadata.displayNames[{3, 1, 1}] = "support";
    const std::vector<std::string> predicates{"true",
                                              "false",
                                              "name ~= \"capillary*\"",
                                              "material == \"Optical\"",
                                              "tag.sensitive",
                                              "tag.sensitive == \"false\"",
                                              "is_leaf",
                                              "path ~= \"**/capillary*/**\"",
                                              "!(material == \"Optical\")",
                                              "name ~= \"core\" || tag.sensitive == \"false\""};
    std::mt19937 random{1438};
    for (int trial = 0; trial < 30; ++trial) {
        std::vector<std::vector<SelectionRule>> passes;
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<SelectionRule> rules;
            for (int i = 0; i < 4; ++i) {
                auto scope = random() % 3 == 0 ? std::optional<std::string>{"/world/tower0/**"}
                                               : std::nullopt;
                rules.push_back(
                    rule(random() % 2 ? SelectionAction::KeepIf : SelectionAction::DropIf,
                         predicates[random() % predicates.size()], scope));
            }
            rules.push_back(rule(SelectionAction::KeepIf, "name == \"world\""));
            passes.push_back(std::move(rules));
        }
        differential(scene, passes, trial % 2 != 0);
    }
}

TEST_CASE("Shared selection skips repeated dropped descendants without materialization",
          "[selection][selection-cache]") {
    constexpr std::size_t towers = 256, capillaries = 32, cores = 128;
    auto scene = cacheFixture(towers);
    auto &children = scene.logVols.at(LogVolId{2}).daughters;
    children.clear();
    for (std::size_t i = 0; i < capillaries; ++i)
        children.push_back({"capillary_" + std::to_string(i), LogVolId{3}, glm::dmat4{1}});
    auto &leaves = scene.logVols.at(LogVolId{3}).daughters;
    leaves.clear();
    for (std::size_t i = 0; i < cores; ++i)
        leaves.push_back({"core" + std::to_string(i), LogVolId{4}, glm::dmat4{1}});
    bool override = false;
    SECTION("identical instances") {}
    SECTION("one exceptional occurrence") {
        override = true;
        scene.metadata.tagSets.push_back({{"force", "keep"}});
        scene.metadata.occurrenceTags[{0, 0, 0}] = scene.metadata.tagSets.size() - 1;
    }
    auto tree = scene.makeTree(0);
    SelectedOccurrences selected{*tree};
    const auto evaluation =
        selected.dryRun({rule(SelectionAction::DropIf, "name ~= \"capillary*\""),
                         rule(SelectionAction::DropIf, "material == \"Optical\""),
                         rule(SelectionAction::KeepIf, "tag.force == \"keep\"")},
                        true);
    REQUIRE(evaluation.keptCount() == 1 + towers + (override ? 1 : 0));
    REQUIRE(evaluation.evaluatedOccurrenceCount() < 2000);
    REQUIRE(evaluation.skippedOccurrenceCount() > 1000000);
    REQUIRE(evaluation.evaluatedOccurrenceCount() + evaluation.skippedOccurrenceCount() ==
            selected.size());
    REQUIRE(evaluation.contains({0, 0, 0}) == override);
    REQUIRE_FALSE(evaluation.contains({towers - 1, capillaries - 1, cores - 1}));
    REQUIRE(tree->materializationCount() == 0);
}

TEST_CASE("Shared selection avoids proof overhead on mostly unique definitions",
          "[selection][selection-cache]") {
    auto scene = cacheFixture(1);
    scene.metadata = {};
    scene.logVols.clear();
    constexpr uint64_t definitions = 128;
    for (uint64_t i = 1; i <= definitions; ++i) {
        scene.logVols.emplace(LogVolId{i},
                              LogicalVolume{LogVolId{i}, "unique", ShapeId{1}, MaterialId{1}});
        if (i > 1)
            scene.logVols.at(LogVolId{1})
                .daughters.push_back({"capillary" + std::to_string(i), LogVolId{i}, glm::dmat4{1}});
    }
    // A small amount of reuse still cannot amortize a graph-wide proof pass.
    scene.logVols.at(LogVolId{1})
        .daughters.push_back({"capillaryAgain", LogVolId{2}, glm::dmat4{1}});
    const std::vector rules{
        rule(SelectionAction::DropIf, "name ~= \"capillary*\" || path ~= \"/world/**\"")};
    auto tree = scene.makeTree(0);
    const auto result = SelectedOccurrences{*tree}.dryRun(rules, true);
    REQUIRE(result.keptCount() == 1);
    REQUIRE(result.evaluatedOccurrenceCount() == definitions + 1);
    REQUIRE(result.skippedOccurrenceCount() == 0);
    differential(scene, {rules}, true);
}

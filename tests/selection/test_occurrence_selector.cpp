#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <diagnostic_codes.hpp>
#include <selection/occurrence_selector.hpp>
#include <selection/selector.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <tuple>

using namespace nodehammer;
using namespace nodehammer::config;
using namespace nodehammer::ir::semantic;
using nodehammer::ir::expanded::Node;
using nodehammer::ir::expanded::NodeId;
using namespace nodehammer::ir::semantic;
using namespace nodehammer::selection;
using namespace nodehammer::selection;

namespace {
struct Fixture {
    nodehammer::ir::expanded::Scene source;
    nodehammer::ir::expanded::Scene expanded;
    DaughterPlacement root{"world", LogVolId{1},
                           glm::translate(glm::dmat4{1.0}, glm::dvec3{3, 4, 5})};
    std::map<OccurrenceId, NodeId> identities;
    std::map<OccurrenceId, OccurrenceTags> tags{
        {{0, 0, 0}, {{"readout", "enabled"}}},
        {{0, 0, 1}, {{"readout", "disabled"}}},
        {{1, 0, 0}, {{"readout", "disabled"}}},
    };

    Fixture() {
        const MaterialId material{1};
        const ShapeId shape{1};
        source.shapes.emplace(shape, Shape{shape, BoxShape{1, 1, 1}});
        source.materials.emplace(material, SourceMaterial{material, "Silicon", std::nullopt, 2.33});
        for (uint64_t i = 1; i <= 4; ++i) {
            source.logVols.emplace(LogVolId{i},
                                   LogicalVolume{LogVolId{i}, "prototype", shape, material});
        }
        const auto left = glm::rotate(glm::translate(glm::dmat4{1.0}, glm::dvec3{10, 0, 0}),
                                      glm::radians(90.0), glm::dvec3{0, 0, 1});
        source.logVols.at(LogVolId{1}).daughters = {
            {"left", LogVolId{2}, left},
            {"right", LogVolId{2}, glm::translate(glm::dmat4{1.0}, glm::dvec3{-10, 0, 0})}};
        source.logVols.at(LogVolId{2}).daughters = {
            {"module", LogVolId{3}, glm::translate(glm::dmat4{1.0}, glm::dvec3{2, 0, 0})}};
        source.logVols.at(LogVolId{3}).daughters = {
            {"sensor", LogVolId{4}, glm::translate(glm::dmat4{1.0}, glm::dvec3{0, 3, 0})},
            {"sensor", LogVolId{4}, glm::translate(glm::dmat4{1.0}, glm::dvec3{0, 7, 0})}};
        expanded = source;
        auto expand = [&](auto &&self, const DaughterPlacement &placement,
                          std::optional<NodeId> parent, OccurrenceId id) -> NodeId {
            Node node;
            node.id = expanded.nextNodeId();
            node.name = placement.name;
            node.logVolId = placement.logVolId;
            node.localTransform = placement.localTransform;
            node.parentId = parent;
            if (const auto it = tags.find(id); it != tags.end()) {
                node.tags = it->second;
            }
            const auto nodeId = node.id;
            expanded.nodes.emplace(nodeId, std::move(node));
            identities.emplace(id, nodeId);
            const auto &daughters = source.logVols.at(placement.logVolId).daughters;
            for (std::size_t i = 0; i < daughters.size(); ++i) {
                auto child = id;
                child.push_back(i);
                const auto childId = self(self, daughters.at(i), nodeId, std::move(child));
                expanded.nodes.at(nodeId).children.push_back(childId);
            }
            return nodeId;
        };
        expanded.rootId = expand(expand, root, std::nullopt, {});
        expanded.computeWorldTransforms();
        expanded.computeOriginalPaths();
    }

    void setTags(OccurrenceTree &tree) const {
        for (const auto &[id, values] : tags) {
            tree.setTags(id, values);
        }
    }
};

SelectionRule rule(SelectionAction action, PredicateExpr predicate,
                   std::optional<std::string> scope = std::nullopt) {
    SelectionRule result;
    result.action = action;
    result.predicate = std::move(predicate);
    result.scope = std::move(scope);
    return result;
}

auto diagnosticContents(const DiagnosticList &diagnostics) {
    std::vector<std::tuple<Diagnostic::Severity, std::string, std::string, std::string>> result;
    for (const auto &diagnostic : diagnostics.items()) {
        result.emplace_back(diagnostic.severity, diagnostic.code, diagnostic.message,
                            diagnostic.context);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void sameMatrix(const glm::dmat4 &actual, const glm::dmat4 &expected) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            REQUIRE(actual[col][row] == Catch::Approx(expected[col][row]).margin(1e-12));
        }
    }
}

void sameSelected(const SelectedOccurrences &selected,
                  const nodehammer::ir::expanded::Scene &expanded,
                  const std::map<OccurrenceId, NodeId> &identities) {
    REQUIRE(selected.size() == expanded.nodes.size());
    for (const auto &[id, nodeId] : identities) {
        CAPTURE(id);
        REQUIRE(selected.contains(id) == expanded.nodes.contains(nodeId));
        if (!expanded.nodes.contains(nodeId)) {
            continue;
        }
        const auto view = selected.materialize(id);
        const auto &node = expanded.nodes.at(nodeId);
        REQUIRE(view.source->id == id);
        REQUIRE(view.source->logVolId == node.logVolId);
        REQUIRE(view.source->name == node.name);
        REQUIRE(view.source->originalPath == node.originalPath);
        REQUIRE(view.source->tags == node.tags);
        REQUIRE(view.isLeaf == node.children.empty());
        REQUIRE(view.parentId.has_value() == node.parentId.has_value());
        if (view.parentId) {
            REQUIRE(identities.at(*view.parentId) == *node.parentId);
        }
        sameMatrix(view.localTransform, node.localTransform);
        sameMatrix(view.source->worldTransform, node.worldTransform);
    }
}

void sameEvaluation(const SelectedOccurrences &selected,
                    const nodehammer::ir::expanded::Scene &expanded,
                    const std::map<OccurrenceId, NodeId> &identities,
                    const std::vector<SelectionRule> &rules, bool hoist) {
    const auto expected = SelectionEngine{rules, hoist}.dryRun(expanded);
    const auto actual = selected.dryRun(rules, hoist);
    REQUIRE(actual.keptCount() == expected.kept.size());
    REQUIRE(actual.droppedCount() == expected.dropped.size());
    REQUIRE(diagnosticContents(actual.diagnostics()) == diagnosticContents(expected.diags));
    for (const auto &[id, nodeId] : identities) {
        CAPTURE(id);
        REQUIRE(actual.contains(id) == expected.kept.contains(nodeId));
    }
}
} // namespace

TEST_CASE("Shared selection agrees with expanded selection per occurrence",
          "[selection][occurrence][differential]") {
    std::vector<SelectionRule> rules;
    SECTION("no rules") {}
    SECTION("exact path matches both duplicate siblings in only one module") {
        rules = {rule(SelectionAction::DropIf,
                      PredicateExpr{PathGlobPredicate{"/world/left/module/sensor"}})};
    }
    SECTION("ordered tag scope and compound predicates distinguish duplicate paths") {
        auto compound = std::make_shared<AndPredicate>();
        compound->operands = {PredicateExpr{MaterialGlobPredicate{"Sil*"}},
                              PredicateExpr{IsLeafPredicate{}},
                              PredicateExpr{TagPredicate{"readout", "enabled"}}};
        rules = {
            rule(SelectionAction::DropIf, PredicateExpr{NameGlobPredicate{"sensor"}}),
            rule(SelectionAction::KeepIf, PredicateExpr{compound}, "/world/left/**"),
            rule(SelectionAction::DropIf, PredicateExpr{TagPredicate{"readout", "enabled"}},
                 "/world/right/**"),
        };
    }
    SECTION("dropping a parent cascades through shared descendants") {
        rules = {rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left"}}),
                 rule(SelectionAction::KeepIf, PredicateExpr{NameGlobPredicate{"sensor"}})};
    }
    for (const auto capacity : {0U, 1U, 4U}) {
        for (const bool hoist : {false, true}) {
            CAPTURE(capacity, hoist);
            Fixture f;
            OccurrenceTree tree{f.source, f.root, capacity};
            f.setTags(tree);
            const SelectedOccurrences original{tree};
            sameEvaluation(original, f.expanded, f.identities, rules, hoist);
            const auto selected = original.prune(rules, hoist);
            const auto expectedDiagnostics = SelectionEngine{rules, hoist}.prune(f.expanded);
            f.expanded.computeWorldTransforms();
            REQUIRE(diagnosticContents(selected.diagnostics()) ==
                    diagnosticContents(expectedDiagnostics));
            sameSelected(selected, f.expanded, f.identities);
            tree.clearCache();
            sameSelected(selected, f.expanded, f.identities);
            REQUIRE(tree.cachedCount() <= capacity);
            REQUIRE(f.source.nodes.empty());
        }
    }
}

TEST_CASE("Shared selection repeated pruning never resurrects source daughters",
          "[selection][occurrence][differential]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 1};
    f.setTags(tree);
    const SelectedOccurrences original{tree};
    const std::vector firstRules{rule(
        SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left/module/sensor"}})};
    const auto first = original.prune(firstRules);
    SelectionEngine{firstRules}.prune(f.expanded);
    tree.clearCache();
    sameSelected(first, f.expanded, f.identities);
    const std::vector secondRules{
        rule(SelectionAction::KeepIf, PredicateExpr{NameGlobPredicate{"sensor"}}),
        rule(SelectionAction::DropIf, PredicateExpr{IsLeafPredicate{}}, "/world/*/module"),
    };
    sameEvaluation(first, f.expanded, f.identities, secondRules, false);
    const auto second = first.prune(secondRules);
    const auto diagnostics = SelectionEngine{secondRules}.prune(f.expanded);
    REQUIRE(diagnosticContents(second.diagnostics()) == diagnosticContents(diagnostics));
    REQUIRE_FALSE(second.contains({0, 0}));
    REQUIRE_FALSE(second.contains({0, 0, 0}));
    REQUIRE(second.contains({1, 0, 0}));
    tree.clearCache();
    sameSelected(second, f.expanded, f.identities);
    REQUIRE(original.size() == 9);
    REQUIRE(original.contains({0, 0, 0}));
    REQUIRE(first.contains({0, 0}));
    REQUIRE(f.source.logVols.at(LogVolId{3}).daughters.size() == 2);
}

TEST_CASE("Shared selection hoisting retains source paths for subsequent rules",
          "[selection][occurrence][differential]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 0};
    f.setTags(tree);
    const SelectedOccurrences original{tree};
    const std::vector firstRules{
        rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left/module"}})};
    const auto first = original.prune(firstRules, true);
    SelectionEngine{firstRules, true}.prune(f.expanded);
    f.expanded.computeWorldTransforms();
    sameSelected(first, f.expanded, f.identities);
    REQUIRE(first.materialize({0, 0, 0}).parentId == std::optional<OccurrenceId>{{0}});
    REQUIRE(first.materialize({0, 0, 0}).source->originalPath == "/world/left/module/sensor");
    const std::vector secondRules{
        rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left"}}),
        rule(SelectionAction::KeepIf,
             PredicateExpr{PathGlobPredicate{"/world/left/module/sensor"}}),
    };
    sameEvaluation(first, f.expanded, f.identities, secondRules, true);
    const auto second = first.prune(secondRules, true);
    SelectionEngine{secondRules, true}.prune(f.expanded);
    f.expanded.computeWorldTransforms();
    sameSelected(second, f.expanded, f.identities);
    REQUIRE(second.materialize({0, 0, 0}).parentId == std::optional<OccurrenceId>{OccurrenceId{}});
}

TEST_CASE("Shared selection root guard and hoist fallback match expanded behavior",
          "[selection][occurrence][differential]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 1};
    f.setTags(tree);
    const SelectedOccurrences original{tree};
    const std::vector rules{
        rule(SelectionAction::DropIf, PredicateExpr{BoolPredicate{true}}),
        rule(SelectionAction::KeepIf, PredicateExpr{TagPredicate{"readout", "enabled"}}),
    };
    sameEvaluation(original, f.expanded, f.identities, rules, false);
    try {
        (void)original.prune(rules);
        FAIL("dropping the root must fail");
    } catch (const Error &error) {
        REQUIRE(error.code() == codes::kFatalSelectionRootDropped);
    }
    REQUIRE(original.size() == 9);
    sameSelected(original, f.expanded, f.identities);
    sameEvaluation(original, f.expanded, f.identities, rules, true);
    const auto selected = original.prune(rules, true);
    SelectionEngine{rules, true}.prune(f.expanded);
    f.expanded.computeWorldTransforms();
    REQUIRE(selected.size() == 2);
    REQUIRE(selected.contains({}));
    REQUIRE(selected.contains({0, 0, 0}));
    sameSelected(selected, f.expanded, f.identities);
}

TEST_CASE("Shared selection handles word boundaries and sparse retained subtrees",
          "[selection][occurrence]") {
    for (const std::size_t total :
         {std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{129}}) {
        CAPTURE(total);
        nodehammer::ir::expanded::Scene source;
        source.logVols.emplace(LogVolId{1}, LogicalVolume{LogVolId{1}, "root", {}, {}});
        source.logVols.emplace(LogVolId{2}, LogicalVolume{LogVolId{2}, "leaf", {}, {}});
        for (std::size_t i = 0; i < total - 1; ++i) {
            source.logVols.at(LogVolId{1})
                .daughters.push_back({"sensor" + std::to_string(i), LogVolId{2}, glm::dmat4{1}});
        }
        OccurrenceTree tree{source, {"world", LogVolId{1}, glm::dmat4{1}}, 1};
        const SelectedOccurrences initial{tree};
        REQUIRE(initial.size() == total);
        REQUIRE(initial.storageBytes() == 0);
        REQUIRE(initial.prune({}).storageBytes() == 0);
        const auto sparse = initial.prune({
            rule(SelectionAction::DropIf, PredicateExpr{BoolPredicate{true}}),
            rule(SelectionAction::KeepIf, PredicateExpr{PathGlobPredicate{"/world"}}),
            rule(SelectionAction::KeepIf,
                 PredicateExpr{PathGlobPredicate{"/world/sensor" + std::to_string(total - 2)}}),
        });
        REQUIRE(sparse.size() == 2);
        REQUIRE_FALSE(sparse.materialize({}).isLeaf);
        REQUIRE(sparse.contains({total - 2}));
        for (std::size_t i = 0; i < total - 2; ++i) {
            REQUIRE_FALSE(sparse.contains({i}));
        }
        const auto rootOnly =
            sparse.prune({rule(SelectionAction::DropIf, PredicateExpr{IsLeafPredicate{}})});
        REQUIRE(rootOnly.size() == 1);
        REQUIRE(rootOnly.materialize({}).isLeaf);
        const auto unchanged = rootOnly.prune({});
        REQUIRE(unchanged.size() == 1);
        REQUIRE_FALSE(unchanged.contains({total - 2}));
        REQUIRE(source.nodes.empty());
    }
}

TEST_CASE("Shared selection rejects cyclic prototypes and occurrence count overflow",
          "[selection][occurrence]") {
    SECTION("cycle") {
        nodehammer::ir::expanded::Scene source;
        source.logVols.emplace(LogVolId{1}, LogicalVolume{LogVolId{1}, "cycle", {}, {}});
        source.logVols.at(LogVolId{1}).daughters.push_back({"self", LogVolId{1}, glm::dmat4{1}});
        OccurrenceTree tree{source, {"world", LogVolId{1}, glm::dmat4{1}}, 0};
        REQUIRE_THROWS_AS(SelectedOccurrences{tree}, std::invalid_argument);
    }
    SECTION("overflow without expanding any nodes") {
        nodehammer::ir::expanded::Scene source;
        for (uint64_t i = 1; i <= 65; ++i) {
            source.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "level", {}, {}});
            if (i != 65) {
                source.logVols.at(LogVolId{i}).daughters = {
                    {"left", LogVolId{i + 1}, glm::dmat4{1}},
                    {"right", LogVolId{i + 1}, glm::dmat4{1}}};
            }
        }
        OccurrenceTree tree{source, {"world", LogVolId{1}, glm::dmat4{1}}, 0};
        REQUIRE_THROWS_AS(SelectedOccurrences{tree}, std::overflow_error);
        REQUIRE(source.nodes.empty());
    }
}

TEST_CASE("Shared selection bounds orphan details while preserving exact warning counts",
          "[selection][occurrence][diagnostics]") {
    const std::vector rules{
        rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left"}})};
    for (const auto limit : {std::optional<uint64_t>{0}, std::optional<uint64_t>{1},
                             std::optional<uint64_t>{3}, std::optional<uint64_t>{std::nullopt}}) {
        CAPTURE(limit);
        Fixture f;
        OccurrenceTree tree{f.source, f.root, 0};
        const SelectedOccurrences original{tree, {.orphanDiagnosticLimit = limit}};
        const auto evaluation = original.dryRun(rules);
        const uint64_t omitted = limit ? 3 - std::min(uint64_t{3}, *limit) : 0;
        REQUIRE(evaluation.orphanWarningCount() == 3);
        REQUIRE(evaluation.omittedOrphanDiagnosticCount() == omitted);
        REQUIRE(evaluation.diagnostics().items().size() == 3 - omitted + (omitted != 0 ? 1 : 0));
        REQUIRE(evaluation.keptCount() == 5);
        REQUIRE(evaluation.droppedCount() == 4);
        if (omitted != 0) {
            const auto &summary = evaluation.diagnostics().items().back();
            REQUIRE(summary.code == codes::kWarnSelectionOrphan);
            REQUIRE(summary.severity == Diagnostic::Severity::Warning);
            REQUIRE(summary.context == "selection");
            REQUIRE(summary.message ==
                    std::to_string(omitted) + " additional orphan warnings omitted (detail limit " +
                        std::to_string(*limit) + "); 3 orphan warnings in total");
        } else {
            REQUIRE(diagnosticContents(evaluation.diagnostics()) ==
                    diagnosticContents(SelectionEngine{rules}.dryRun(f.expanded).diags));
        }
        const auto selected = original.prune(rules);
        REQUIRE(selected.orphanWarningCount() == 3);
        REQUIRE(selected.omittedOrphanDiagnosticCount() == omitted);
        REQUIRE(diagnosticContents(selected.diagnostics()) ==
                diagnosticContents(evaluation.diagnostics()));
        const auto hoistedEvaluation = original.dryRun(rules, true);
        REQUIRE(hoistedEvaluation.orphanWarningCount() == 0);
        REQUIRE(hoistedEvaluation.omittedOrphanDiagnosticCount() == 0);
        REQUIRE(hoistedEvaluation.diagnostics().empty());
        const auto hoisted = original.prune(rules, true);
        REQUIRE(hoisted.orphanWarningCount() == 0);
        REQUIRE(hoisted.omittedOrphanDiagnosticCount() == 0);
        REQUIRE(hoisted.diagnostics().empty());
    }
}

TEST_CASE("Shared selection diagnostic counts describe each run independently",
          "[selection][occurrence][diagnostics]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 1};
    const SelectedOccurrences original{tree, {.orphanDiagnosticLimit = 1}};
    const auto first = original.prune(
        {rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left"}})});
    REQUIRE(first.orphanWarningCount() == 3);
    REQUIRE(first.omittedOrphanDiagnosticCount() == 2);
    const auto second = first.prune(
        {rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/right"}})});
    REQUIRE(second.orphanWarningCount() == 3);
    REQUIRE(second.omittedOrphanDiagnosticCount() == 2);
    REQUIRE(second.diagnostics().items().size() == 2);
    REQUIRE(second.size() == 1);
    const auto unchanged = first.prune({});
    REQUIRE(unchanged.size() == first.size());
    REQUIRE(unchanged.orphanWarningCount() == 0);
    REQUIRE(unchanged.omittedOrphanDiagnosticCount() == 0);
    REQUIRE(unchanged.diagnostics().empty());
    const auto evaluation = first.dryRun({});
    REQUIRE(evaluation.orphanWarningCount() == 0);
    REQUIRE(evaluation.omittedOrphanDiagnosticCount() == 0);
    REQUIRE(evaluation.diagnostics().empty());
    const auto nonmatching =
        first.prune({rule(SelectionAction::DropIf, PredicateExpr{NameGlobPredicate{"absent"}})});
    REQUIRE(nonmatching.orphanWarningCount() == 0);
    REQUIRE(nonmatching.omittedOrphanDiagnosticCount() == 0);
    REQUIRE(nonmatching.diagnostics().empty());
    REQUIRE(first.orphanWarningCount() == 3);
    REQUIRE(first.omittedOrphanDiagnosticCount() == 2);
}

TEST_CASE("Shared selection root error retains bounded orphan summary",
          "[selection][occurrence][diagnostics]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 0};
    const SelectedOccurrences original{tree, {.orphanDiagnosticLimit = 0}};
    const std::vector rules{
        rule(SelectionAction::DropIf, PredicateExpr{NameGlobPredicate{"world"}})};
    const auto evaluation = original.dryRun(rules);
    REQUIRE(evaluation.orphanWarningCount() == 8);
    REQUIRE(evaluation.omittedOrphanDiagnosticCount() == 8);
    try {
        (void)original.prune(rules);
        FAIL("dropping the root must fail");
    } catch (const Error &error) {
        REQUIRE(error.code() == codes::kFatalSelectionRootDropped);
        REQUIRE(error.observed().size() == 1);
        REQUIRE(error.observed().front().code == codes::kWarnSelectionOrphan);
        REQUIRE(
            error.observed().front().message ==
            "8 additional orphan warnings omitted (detail limit 0); 8 orphan warnings in total");
    }
    REQUIRE(original.orphanWarningCount() == 0);
    REQUIRE(original.size() == 9);
}

TEST_CASE("Shared selection default diagnostic storage is bounded for many orphan descendants",
          "[selection][occurrence][diagnostics]") {
    nodehammer::ir::expanded::Scene source;
    for (uint64_t i = 1; i <= 3; ++i) {
        source.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "prototype", {}, {}});
    }
    source.logVols.at(LogVolId{1}).daughters = {{"module", LogVolId{2}, glm::dmat4{1}}};
    for (std::size_t i = 0; i < 10000; ++i) {
        source.logVols.at(LogVolId{2}).daughters.push_back({"sensor", LogVolId{3}, glm::dmat4{1}});
    }
    OccurrenceTree tree{source, {"world", LogVolId{1}, glm::dmat4{1}}, 0};
    const SelectedOccurrences original{tree};
    const auto selected =
        original.prune({rule(SelectionAction::DropIf, PredicateExpr{NameGlobPredicate{"module"}})});
    REQUIRE(selected.size() == 1);
    REQUIRE(selected.orphanWarningCount() == 10000);
    REQUIRE(selected.omittedOrphanDiagnosticCount() == 8976);
    REQUIRE(selected.diagnostics().items().size() == 1025);
    REQUIRE(selected.diagnostics().items().back().message ==
            "8976 additional orphan warnings omitted (detail limit 1024); 10000 orphan warnings in "
            "total");
    std::size_t messageBytes = 0;
    for (const auto &diagnostic : selected.diagnostics().items()) {
        messageBytes +=
            diagnostic.message.size() + diagnostic.context.size() + diagnostic.code.size();
    }
    REQUIRE(messageBytes < 100000);
    REQUIRE(selected.storageBytes() < 3000);
    REQUIRE(tree.materializationCount() == 0);
    REQUIRE(source.nodes.empty());
}

TEST_CASE("Shared selection uses reusable metadata and display names with original paths",
          "[selection][occurrence]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 1};
    tree.setVolumeTags(LogVolId{4}, {{"sensitive", "true"}, {"readout", "base"}});
    tree.setPlacementTags(LogVolId{3}, 0, {{"readout", "first"}});
    tree.setTags({1, 0, 0}, {});
    tree.setDisplayName({0, 0, 0}, "PixelSensor");
    // Populate the expanded oracle independently, without copying effective tags
    // out of the implementation under test.
    for (const auto &[id, nodeId] : f.identities) {
        auto &node = f.expanded.nodes.at(nodeId);
        node.tags.clear();
        if (id.size() == 3) {
            node.tags = {{"sensitive", "true"}, {"readout", id.back() == 0 ? "first" : "base"}};
        }
    }
    f.expanded.nodes.at(f.identities.at({1, 0, 0})).tags.clear();
    f.expanded.nodes.at(f.identities.at({0, 0, 0})).name = "PixelSensor";
    const SelectedOccurrences shared{tree};
    const std::vector rules{
        rule(SelectionAction::DropIf, PredicateExpr{BoolPredicate{true}}),
        rule(SelectionAction::KeepIf, PredicateExpr{NameGlobPredicate{"world"}}),
        rule(SelectionAction::KeepIf, PredicateExpr{TagPredicate{"sensitive", "true"}},
             "/world/left/**"),
        rule(SelectionAction::DropIf, PredicateExpr{NameGlobPredicate{"PixelSensor"}}),
    };
    sameEvaluation(shared, f.expanded, f.identities, rules, true);
    const auto selected = shared.prune(rules, true);
    SelectionEngine{rules, true}.prune(f.expanded);
    sameSelected(selected, f.expanded, f.identities);
    REQUIRE(selected.contains({0, 0, 1}));
    REQUIRE_FALSE(selected.contains({0, 0, 0}));
    REQUIRE(tree.tagOverrideCount() == 1);
}

TEST_CASE("Shared selection root diagnostics use display names without changing raw paths",
          "[selection][occurrence]") {
    Fixture f;
    OccurrenceTree tree{f.source, f.root, 0};
    tree.setDisplayName({}, "DetectorWorld");
    f.expanded.nodes.at(f.expanded.rootId).name = "DetectorWorld";
    const SelectedOccurrences selected{tree};
    const std::vector rules{
        rule(SelectionAction::DropIf, PredicateExpr{NameGlobPredicate{"DetectorWorld"}})};
    sameEvaluation(selected, f.expanded, f.identities, rules, false);
    REQUIRE(selected.materialize({}).source->originalPath == "/world");
    try {
        (void)selected.prune(rules);
        FAIL("dropping the renamed root must fail");
    } catch (const Error &error) {
        REQUIRE(error.code() == codes::kFatalSelectionRootDropped);
        REQUIRE(error.context() == "DetectorWorld");
    }
}

TEST_CASE("Canonical selection projects retained topology and supports successive pruning",
          "[selection][occurrence][differential]") {
    Fixture fixture;
    nodehammer::ir::semantic::Scene canonical;
    static_cast<GeometryCatalogs &>(canonical) = fixture.source;
    canonical.root = fixture.root;
    for (const auto &[id, tags] : fixture.tags) {
        canonical.metadata.occurrenceTags[id] = canonical.metadata.tagSets.size();
        canonical.metadata.tagSets.push_back(tags);
    }
    auto reference = fixture.expanded;
    bool hoist = false;
    SECTION("drop subtrees") {}
    SECTION("hoist across a rotated parent") { hoist = true; }
    const std::vector<std::vector<SelectionRule>> passes{
        {rule(SelectionAction::DropIf, PredicateExpr{PathGlobPredicate{"/world/left/module"}})},
        {rule(SelectionAction::DropIf, PredicateExpr{TagPredicate{"readout", "disabled"}})}};
    for (const auto &rules : passes) {
        const SelectionEngine engine{rules, hoist};
        const auto expectedDiagnostics = engine.prune(reference);
        const auto actualDiagnostics = engine.prune(canonical);
        REQUIRE(diagnosticContents(actualDiagnostics) == diagnosticContents(expectedDiagnostics));
        REQUIRE(canonical.nodeCount() == reference.nodes.size());
        auto tree = canonical.makeTree(1);
        struct Pair {
            OccurrenceId occurrence;
            NodeId node;
        };
        std::vector<Pair> pending{{{}, reference.rootId}};
        while (!pending.empty()) {
            auto pair = std::move(pending.back());
            pending.pop_back();
            const auto actual = tree->materialize(pair.occurrence);
            const auto &expected = reference.nodes.at(pair.node);
            CHECK(actual->name == expected.name);
            CHECK(actual->originalPath == expected.originalPath);
            CHECK(actual->tags == expected.tags);
            CHECK(actual->sourceSystem == expected.sourceSystem);
            CHECK(actual->degradation.bits == expected.degradation.bits);
            sameMatrix(actual->localTransform, expected.localTransform);
            sameMatrix(actual->worldTransform, expected.worldTransform);
            REQUIRE(actual->childCount == expected.children.size());
            for (std::size_t i = 0; i < expected.children.size(); ++i) {
                auto child = pair.occurrence;
                child.push_back(i);
                pending.push_back({std::move(child), expected.children[i]});
            }
        }
    }
}

// Selection-only baseline. Fixture construction and scene copies are outside timers.
// prune() includes its own evaluation: do not subtract dryRun() to infer prune cost.
#include <selection/occurrence_selector.hpp>
#include <selection/selector.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
using namespace nodehammer;
using namespace nodehammer::config;
using namespace nodehammer::ir::semantic;
using nodehammer::ir::expanded::Node;
using nodehammer::ir::expanded::NodeId;
using Clock = std::chrono::steady_clock;

std::size_t positive(std::string_view text) {
    std::size_t result{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (ec != std::errc{} || ptr != text.data() + text.size() || result == 0) {
        throw std::runtime_error("arguments must be positive integers");
    }
    return result;
}

nodehammer::ir::expanded::Scene fixture(std::size_t modules, std::size_t fibers) {
    nodehammer::ir::expanded::Scene scene;
    scene.nodes.reserve(1 + modules * (fibers + 1));
    const auto shape = scene.nextShapeId();
    const auto material = scene.nextMaterialId();
    scene.shapes.emplace(shape, Shape{shape, BoxShape{1, 1, 1}});
    scene.materials.emplace(material, SourceMaterial{material, "Silicon", std::nullopt, 2.33});
    const auto worldLv = scene.nextLogVolId();
    const auto moduleLv = scene.nextLogVolId();
    const auto fiberLv = scene.nextLogVolId();
    scene.logVols.emplace(worldLv, LogicalVolume{worldLv, "world", shape, material});
    scene.logVols.emplace(moduleLv, LogicalVolume{moduleLv, "module", shape, material});
    scene.logVols.emplace(fiberLv, LogicalVolume{fiberLv, "fiber", shape, material});
    for (std::size_t f = 0; f < fibers; ++f) {
        scene.logVols.at(moduleLv).daughters.push_back(
            {"fiber_" + std::to_string(f), fiberLv,
             glm::translate(glm::dmat4{1.0}, glm::dvec3{0, double(f), 0})});
    }
    Node root;
    root.id = scene.nextNodeId();
    root.name = "world";
    root.logVolId = worldLv;
    scene.rootId = root.id;
    scene.nodes.emplace(root.id, std::move(root));
    for (std::size_t m = 0; m < modules; ++m) {
        Node module;
        module.id = scene.nextNodeId();
        module.name = "module_" + std::to_string(m);
        module.logVolId = moduleLv;
        module.parentId = scene.rootId;
        module.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{double(m), 0, 0});
        scene.logVols.at(worldLv).daughters.push_back(
            {module.name, moduleLv, module.localTransform});
        const auto moduleId = module.id;
        scene.nodes.at(scene.rootId).children.push_back(moduleId);
        scene.nodes.emplace(moduleId, std::move(module));
        for (std::size_t f = 0; f < fibers; ++f) {
            Node fiber;
            fiber.id = scene.nextNodeId();
            fiber.name = "fiber_" + std::to_string(f);
            fiber.logVolId = fiberLv;
            fiber.parentId = moduleId;
            fiber.localTransform = scene.logVols.at(moduleLv).daughters.at(f).localTransform;
            fiber.tags.emplace("sensitive", f % 2 == 0 ? "true" : "false");
            scene.nodes.at(moduleId).children.push_back(fiber.id);
            scene.nodes.emplace(fiber.id, std::move(fiber));
        }
    }
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

struct Scenario {
    std::string name;
    std::vector<SelectionRule> rules;
    bool hoist{};
    bool scoped{};
};

bool expected(const Node &node, const Scenario &scenario) {
    if (node.name.starts_with("module_")) {
        return !scenario.hoist;
    }
    if (!node.name.starts_with("fiber_")) {
        return true;
    }
    if (scenario.scoped && node.originalPath.starts_with("/world/module_0/")) {
        return node.name == "fiber_0";
    }
    return node.tags.at("sensitive") == "true";
}

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

nlohmann::json timings(std::vector<double> samples) {
    auto sorted = samples;
    std::ranges::sort(sorted);
    const auto n = sorted.size();
    return {{"samples_ms", samples},
            {"median_ms", (sorted.at(n / 2) + sorted.at((n - 1) / 2)) / 2}};
}

nlohmann::json peakRss() {
#if defined(__linux__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        return usage.ru_maxrss;
#else
        return std::uint64_t(usage.ru_maxrss) * 1024;
#endif
    }
#endif
    return nullptr;
}

nlohmann::json run(const nodehammer::ir::expanded::Scene &source, const Scenario &scenario,
                   std::size_t runs, std::size_t warmups) {
    selection::SelectionEngine engine{scenario.rules, scenario.hoist};
    std::vector<double> evaluateTimes, pruneTimes;
    std::size_t kept{};
    for (std::size_t iteration = 0; iteration < warmups + runs; ++iteration) {
        const auto start = Clock::now();
        const auto selected = engine.dryRun(source);
        const auto evaluationMs = milliseconds(start);
        require(selected.diags.empty(), "unexpected dryRun diagnostics");
        require(selected.kept.size() + selected.dropped.size() == source.nodes.size(),
                "selection partition size mismatch");
        for (const auto &[id, node] : source.nodes) {
            require(selected.kept.contains(id) == expected(node, scenario),
                    "incorrect selected occurrence");
            require(selected.dropped.contains(id) != expected(node, scenario),
                    "incorrect dropped occurrence");
        }
        auto output = source; // Excluded from timing; included in process RSS high-water.
        const auto pruneStart = Clock::now();
        const auto diagnostics = engine.prune(output);
        const auto pruneMs = milliseconds(pruneStart);
        require(diagnostics.empty(), "unexpected prune diagnostics");
        require(output.nodes.size() == selected.kept.size(), "pruned node count mismatch");
        for (const auto &[id, node] : output.nodes) {
            require(selected.kept.contains(id), "incorrect pruned occurrence");
            const auto &original = source.nodes.at(id);
            require(node.originalPath == original.originalPath, "original path changed");
            require(node.worldTransform == original.worldTransform, "world transform changed");
            require(node.parentId == (scenario.hoist && id != source.rootId
                                          ? std::optional{source.rootId}
                                          : original.parentId),
                    "incorrect output parent");
        }
        kept = output.nodes.size();
        if (iteration >= warmups) {
            evaluateTimes.push_back(evaluationMs);
            pruneTimes.push_back(pruneMs);
        }
    }
    return {{"name", scenario.name},
            {"rule_count", scenario.rules.size()},
            {"hoist_orphans", scenario.hoist},
            {"kept_nodes", kept},
            {"dry_run", timings(evaluateTimes)},
            {"prune_including_evaluation", timings(pruneTimes)},
            {"process_peak_rss_bytes", peakRss()}};
}

// Reference Scene is retained for correctness checking, not consulted by the
// shared representation. RSS therefore cannot measure shared-only memory here.
nlohmann::json runShared(const nodehammer::ir::expanded::Scene &source, const Scenario &scenario,
                         std::size_t modules, std::size_t fibers, std::size_t runs,
                         std::size_t warmups) {
    namespace occurrence = ir::semantic;
    nodehammer::ir::expanded::Scene prototypes;
    prototypes.logVols = source.logVols;
    prototypes.shapes = source.shapes;
    prototypes.materials = source.materials;
    const auto &root = source.nodes.at(source.rootId);
    constexpr std::size_t capacity = 64;
    occurrence::OccurrenceTree tree(prototypes, {root.name, root.logVolId, root.localTransform},
                                    capacity);
    // All module occurrences reuse the same daughter metadata. Resolve the
    // prototype from the root placement instead of relying on fixture IDs.
    const auto moduleLv = prototypes.logVols.at(root.logVolId).daughters.at(0).logVolId;
    for (std::size_t f = 0; f < fibers; ++f) {
        tree.setPlacementTags(moduleLv, f, {{"sensitive", f % 2 == 0 ? "true" : "false"}});
    }
    selection::SelectedOccurrences shared(tree);
    std::vector<double> evaluateTimes, pruneTimes;
    std::size_t kept{}, selectedStorage{}, selectionMaterializations{};
    auto checkOccurrence = [&](const occurrence::OccurrenceId &id, const Node &original,
                               const auto &evaluation, const auto &output) {
        const bool keep = expected(original, scenario);
        require(evaluation.contains(id) == keep, "shared evaluation occurrence mismatch");
        require(output.contains(id) == keep, "shared pruned occurrence mismatch");
        if (!keep) {
            return;
        }
        const auto node = output.materialize(id);
        require(node.source->tags == original.tags, "shared source metadata mismatch");
        require(node.source->originalPath == original.originalPath,
                "shared original path mismatch");
        require(node.source->worldTransform == original.worldTransform,
                "shared world transform mismatch");
        std::optional<occurrence::OccurrenceId> parent;
        if (!id.empty()) {
            parent = scenario.hoist ? occurrence::OccurrenceId{} : id;
            if (!scenario.hoist) {
                parent->pop_back();
            }
        }
        require(node.parentId == parent, "shared selected parent mismatch");
        require(node.isLeaf == original.children.empty(), "shared selected leaf mismatch");
        require(node.localTransform == (scenario.hoist && !id.empty() ? original.worldTransform
                                                                      : original.localTransform),
                "shared selected local transform mismatch");
    };
    for (std::size_t iteration = 0; iteration < runs + warmups; ++iteration) {
        tree.clearCache(); // Validation from earlier runs must not warm the measured traversal.
        const auto before = tree.materializationCount();
        const auto start = Clock::now();
        const auto evaluation = shared.dryRun(scenario.rules, scenario.hoist);
        const auto evaluationMs = milliseconds(start);
        const auto pruneStart = Clock::now();
        auto output = shared.prune(scenario.rules, scenario.hoist);
        const auto pruneMs = milliseconds(pruneStart);
        selectionMaterializations = tree.materializationCount() - before;
        require(selectionMaterializations == 0, "shared selection materialized occurrences");
        require(evaluation.diagnostics().empty(), "shared evaluation diagnostics");
        require(output.diagnostics().empty(), "shared prune diagnostics");
        require(evaluation.keptCount() + evaluation.droppedCount() == source.nodes.size(),
                "shared partition size mismatch");
        require(output.size() == evaluation.keptCount(), "shared pruned size mismatch");
        checkOccurrence({}, root, evaluation, output);
        for (std::size_t m = 0; m < modules; ++m) {
            const auto moduleId = root.children.at(m);
            const auto &module = source.nodes.at(moduleId);
            checkOccurrence({m}, module, evaluation, output);
            for (std::size_t f = 0; f < fibers; ++f) {
                checkOccurrence({m, f}, source.nodes.at(module.children.at(f)), evaluation, output);
            }
        }
        require(prototypes.nodes.empty(), "shared selector expanded prototype scene");
        require(tree.cachedCount() <= capacity, "shared materialization cache exceeded capacity");
        kept = output.size();
        selectedStorage = output.storageBytes();
        if (iteration >= warmups) {
            evaluateTimes.push_back(evaluationMs);
            pruneTimes.push_back(pruneMs);
        }
    }
    std::size_t daughters{};
    for (const auto &[id, volume] : prototypes.logVols) {
        daughters += volume.daughters.size();
    }
    return {{"kept_nodes", kept},
            {"dry_run", timings(evaluateTimes)},
            {"prune_including_evaluation", timings(pruneTimes)},
            {"selected_storage_bytes", selectedStorage},
            {"prototype_volume_count", prototypes.logVols.size()},
            {"prototype_daughter_count", daughters},
            {"prototype_expanded_node_count", prototypes.nodes.size()},
            {"persistent_tag_override_count", tree.tagOverrideCount()},
            {"prototype_placement_tag_count", tree.placementTagDefaultCount()},
            {"prototype_volume_tag_count", tree.volumeTagDefaultCount()},
            {"interned_tag_set_count", tree.internedTagSetCount()},
            {"cache_capacity", capacity},
            {"cached_occurrences_after_validation", tree.cachedCount()},
            {"selection_materializations_last_run", selectionMaterializations},
            {"process_peak_rss_bytes", peakRss()},
            {"memory_note",
             "Selected storage is mask/rank array payload only, not total heap. "
             "Prototype tables, reusable placement tag maps, cached snapshots, and the "
             "expanded correctness reference also consume memory. RSS includes all of them."}};
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc > 5) {
            throw std::runtime_error(
                "usage: nodehammer_selection_bench [modules] [fibers] [runs] [warmups]");
        }
        const auto modules = argc > 1 ? positive(argv[1]) : 256;
        const auto fibers = argc > 2 ? positive(argv[2]) : 1024;
        const auto runs = argc > 3 ? positive(argv[3]) : 5;
        const auto warmups = argc > 4 ? positive(argv[4]) : 1;
        require(fibers < std::numeric_limits<std::size_t>::max() &&
                    modules <= (std::numeric_limits<std::size_t>::max() - 1) / (fibers + 1),
                "fixture size overflow");
        require(runs <= std::numeric_limits<std::size_t>::max() - warmups, "run count overflow");
        const SelectionRule dropOdd{SelectionAction::DropIf, std::nullopt,
                                    PredicateExpr{TagPredicate{"sensitive", "false"}}};
        const std::vector<Scenario> scenarios{
            {"tag", {dropOdd}},
            {"scope_order",
             {{SelectionAction::DropIf, std::nullopt, PredicateExpr{NameGlobPredicate{"fiber_*"}}},
              {SelectionAction::KeepIf, std::nullopt,
               PredicateExpr{TagPredicate{"sensitive", "true"}}},
              {SelectionAction::DropIf, "/world/module_0/**", PredicateExpr{IsLeafPredicate{}}},
              {SelectionAction::KeepIf, std::nullopt,
               PredicateExpr{PathGlobPredicate{"/world/module_0/fiber_0"}}}},
             false,
             true},
            {"hoist",
             {dropOdd,
              {SelectionAction::DropIf, std::nullopt,
               PredicateExpr{NameGlobPredicate{"module_*"}}}},
             true}};
        const auto source = fixture(modules, fibers);
        nlohmann::json result{
            {"schema_version", 2},
            {"representation", "expanded_vs_shared"},
            {"modules", modules},
            {"fibers_per_module", fibers},
            {"input_nodes", source.nodes.size()},
            {"runs", runs},
            {"warmups", warmups},
            {"rss_note", "Cumulative process high-water; includes fixture, source copy, validation "
                         "and earlier scenarios. Not stage allocation or live cache size."},
            {"scenarios", nlohmann::json::array()}};
        for (const auto &scenario : scenarios) {
            auto expanded = run(source, scenario, runs, warmups);
            auto shared = runShared(source, scenario, modules, fibers, runs, warmups);
            require(expanded.at("kept_nodes") == shared.at("kept_nodes"),
                    "expanded/shared kept count mismatch");
            result["scenarios"].push_back({{"name", scenario.name},
                                           {"expanded", std::move(expanded)},
                                           {"shared", std::move(shared)}});
        }
        std::cout << result.dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "selection benchmark: " << error.what() << '\n';
        return 1;
    }
}

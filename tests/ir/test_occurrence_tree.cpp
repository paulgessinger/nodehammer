#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ir/expanded/scene.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <selection/predicate.hpp>

#include <glm/gtc/matrix_transform.hpp>

using namespace nodehammer::ir::semantic;
using nodehammer::ir::expanded::Node;
using nodehammer::ir::expanded::NodeId;
using namespace nodehammer::ir::semantic;

namespace {
nodehammer::ir::expanded::Scene prototypes(std::size_t modules = 2, std::size_t sensors = 2) {
    nodehammer::ir::expanded::Scene scene;
    const MaterialId material{1};
    scene.materials.emplace(material, SourceMaterial{material, "Silicon", std::nullopt, 2.33});
    for (uint64_t i = 1; i <= 3; ++i) {
        scene.logVols.emplace(LogVolId{i}, LogicalVolume{LogVolId{i}, "prototype", {}, material});
    }
    for (std::size_t i = 0; i < modules; ++i) {
        auto transform =
            glm::translate(glm::dmat4{1.0}, glm::dvec3{10.0 * static_cast<double>(i), 0, 0});
        if (i == 1) {
            transform = glm::rotate(transform, std::numbers::pi / 2, glm::dvec3{0, 0, 1});
        }
        scene.logVols.at(LogVolId{1})
            .daughters.push_back({"module" + std::to_string(i), LogVolId{2}, transform});
    }
    for (std::size_t i = 0; i < sensors; ++i) {
        scene.logVols.at(LogVolId{2})
            .daughters.push_back(
                {"sensor", LogVolId{3},
                 glm::translate(glm::dmat4{1.0}, glm::dvec3{double(i + 1), 0, 0})});
    }
    return scene;
}
DaughterPlacement root() { return {"world", LogVolId{1}, glm::dmat4{1.0}}; }
} // namespace

TEST_CASE("Occurrence cache retains identities and transforms across eviction", "[occurrence]") {
    auto source = prototypes();
    OccurrenceTree tree(source, root(), 1);
    auto first = tree.materialize({1, 0});
    REQUIRE(first->originalPath == "/world/module1/sensor");
    REQUIRE(first->worldTransform[3][0] == Catch::Approx(10));
    REQUIRE(first->worldTransform[3][1] == Catch::Approx(1));
    REQUIRE(tree.materialize({1, 0}) == first);
    REQUIRE(tree.cacheHitCount() == 1);
    auto sibling = tree.materialize({1, 1});
    REQUIRE(sibling->originalPath == first->originalPath);
    REQUIRE(sibling->id != first->id); // Duplicate sibling names do not alias identity.
    REQUIRE(tree.cachedCount() == 1);
    auto rebuilt = tree.materialize({1, 0});
    REQUIRE(rebuilt != first);
    REQUIRE(rebuilt->id == first->id);
    REQUIRE(rebuilt->worldTransform == first->worldTransform);
    REQUIRE(first->originalPath == "/world/module1/sensor"); // Pinned snapshot still alive.
    REQUIRE(source.nodes.empty());
}

TEST_CASE("Occurrence tags survive eviction without leaking to sibling instances", "[occurrence]") {
    auto source = prototypes();
    OccurrenceTree tree(source, root(), 1);
    auto before = tree.materialize({0, 0});
    tree.setTags({0, 0}, {{"selected", "yes"}});
    auto edited = tree.materialize({0, 0});
    REQUIRE(before->tags.empty()); // Explicit immutable snapshot semantics.
    REQUIRE(edited->tags.at("selected") == "yes");
    tree.clearCache();
    REQUIRE(tree.materialize({1, 0})->tags.empty());
    REQUIRE(tree.materialize({0, 0})->tags.at("selected") == "yes");
    tree.setTags({0, 0}, {});
    REQUIRE(tree.materialize({0, 0})->tags.empty());
    REQUIRE(edited->tags.at("selected") == "yes");
}

TEST_CASE("Occurrence traversal bounds cache and can skip a repeated branch", "[occurrence]") {
    auto source = prototypes(100, 100);
    OccurrenceTree tree(source, root(), 4);
    std::size_t count = 0;
    tree.visit([&](const Occurrence &) {
        ++count;
        REQUIRE(tree.cachedCount() <= 4);
        return true;
    });
    REQUIRE(count == 10101);
    REQUIRE(tree.materializationCount() == count);
    REQUIRE(source.nodes.empty());
    count = 0;
    tree.visit([&](const Occurrence &occurrence) {
        ++count;
        return occurrence.id != OccurrenceId{0};
    });
    REQUIRE(count == 10001);
    REQUIRE(source.logVols.size() == 3);
}

TEST_CASE("Occurrence predicates distinguish paths tags material and leaf status", "[occurrence]") {
    using namespace nodehammer::selection;
    auto source = prototypes();
    OccurrenceTree tree(source, root(), 0); // Streaming with no retained cache entries.
    tree.setTags({1, 0}, {{"sensitive", "true"}});
    auto predicate = makeAndPredicate(
        {makePathGlobPredicate("/world/module1/*"), makeMaterialGlobPredicate("Silicon"),
         makeTagPredicate("sensitive", "true"), makeIsLeafPredicate()});
    std::vector<OccurrenceId> selected;
    tree.visit([&](const Occurrence &o) {
        NodeView view{o.name, o.originalPath, o.materialName, o.childCount == 0, &o.tags};
        if (predicate(view)) {
            selected.push_back(o.id);
        }
        REQUIRE(tree.cachedCount() == 0);
        return true;
    });
    REQUIRE(selected == std::vector<OccurrenceId>{{1, 0}});
}

TEST_CASE("Occurrence resolution rejects invalid addresses and prototype cycles", "[occurrence]") {
    auto source = prototypes();
    OccurrenceTree tree(source, root(), 2);
    REQUIRE_THROWS_AS(tree.materialize({2}), std::out_of_range);
    REQUIRE_THROWS_AS(tree.setTags({0, 2}, {{"bad", "tag"}}), std::out_of_range);
    REQUIRE(tree.cachedCount() == 0);
    auto cyclic = prototypes();
    cyclic.logVols.at(LogVolId{3}).daughters.push_back({"cycle", LogVolId{1}, glm::dmat4{1.0}});
    OccurrenceTree cycle(cyclic, root(), 2);
    REQUIRE_THROWS_AS(cycle.visit([](const Occurrence &) { return true; }), std::invalid_argument);
}

TEST_CASE("Occurrence snapshots agree with expanded scene paths and transforms", "[occurrence]") {
    auto source = prototypes(4, 5);
    std::map<OccurrenceId, NodeId> identities;
    auto expand = [&](auto &&self, const DaughterPlacement &placement, std::optional<NodeId> parent,
                      OccurrenceId id) -> NodeId {
        Node node;
        node.id = source.nextNodeId();
        node.name = placement.name;
        node.logVolId = placement.logVolId;
        node.localTransform = placement.localTransform;
        node.parentId = parent;
        const auto nodeId = node.id;
        source.nodes.emplace(nodeId, std::move(node));
        identities.emplace(id, nodeId);
        const auto &daughters = source.logVols.at(placement.logVolId).daughters;
        for (std::size_t i = 0; i < daughters.size(); ++i) {
            auto childId = id;
            childId.push_back(i);
            const auto child = self(self, daughters.at(i), nodeId, std::move(childId));
            source.nodes.at(nodeId).children.push_back(child);
        }
        return nodeId;
    };
    auto placement = root();
    placement.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{3, 4, 5});
    source.rootId = expand(expand, placement, std::nullopt, {});
    source.computeWorldTransforms();
    source.computeOriginalPaths();
    for (const auto capacity : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
        OccurrenceTree tree(source, placement, capacity);
        std::size_t count = 0;
        tree.visit([&](const Occurrence &o) {
            const auto &node = source.nodes.at(identities.at(o.id));
            REQUIRE(o.originalPath == node.originalPath);
            REQUIRE(o.name == node.name);
            REQUIRE(o.logVolId == node.logVolId);
            REQUIRE(o.childCount == node.children.size());
            REQUIRE(o.localTransform == node.localTransform);
            REQUIRE(o.worldTransform == node.worldTransform);
            ++count;
            return true;
        });
        REQUIRE(count == source.nodes.size());
    }
}

TEST_CASE("Occurrence metadata shares defaults and isolates complete-map overrides",
          "[occurrence]") {
    auto source = prototypes(100, 2);
    OccurrenceTree tree(source, root(), 1);
    tree.setVolumeTags(LogVolId{3}, {{"sensitive", "true"}, {"readout", "default"}});
    tree.setPlacementTags(LogVolId{2}, 0, {{"readout", "left"}, {"layer", "0"}});
    const auto &shared = tree.tagsFor({0, 0});
    REQUIRE(shared.at("sensitive") == "true");
    REQUIRE(shared.at("readout") == "left");
    REQUIRE(shared.at("layer") == "0");
    for (std::size_t module = 1; module < 100; ++module) {
        REQUIRE(&tree.tagsFor({module, 0}) == &shared);
        REQUIRE(tree.tagsFor({module, 1}).at("readout") == "default");
    }
    REQUIRE(tree.internedTagSetCount() == 3);
    REQUIRE(tree.volumeTagDefaultCount() == 1);
    REQUIRE(tree.placementTagDefaultCount() == 1);
    REQUIRE(tree.tagOverrideCount() == 0);
    auto before = tree.materialize({0, 0});
    tree.setTags({0, 0}, {}); // Empty means intentionally clear inherited defaults.
    REQUIRE(tree.tagsFor({0, 0}).empty());
    REQUIRE(tree.materialize({0, 0})->tags.empty());
    REQUIRE(tree.tagsFor({1, 0}).at("sensitive") == "true");
    REQUIRE(before->tags == shared);
    tree.clearCache();
    REQUIRE(tree.tagsFor({0, 0}).empty());
    tree.clearTagOverride({0, 0});
    REQUIRE(&tree.tagsFor({0, 0}) == &shared);
    tree.setTags({1, 0}, {{"special", "only"}});
    REQUIRE(tree.tagsFor({1, 0}).size() == 1);
    tree.setTags({2, 0}, {{"special", "only"}});
    REQUIRE(&tree.tagsFor({1, 0}) == &tree.tagsFor({2, 0}));
}

TEST_CASE("Changing metadata defaults invalidates snapshots without changing pinned values",
          "[occurrence]") {
    auto source = prototypes();
    OccurrenceTree tree(source, root(), 3);
    tree.setVolumeTags(LogVolId{3}, {{"sensitive", "true"}});
    tree.setPlacementTags(LogVolId{2}, 0, {{"layer", "0"}});
    auto before = tree.materialize({0, 0});
    const auto &oldTags = tree.tagsFor({0, 0});
    tree.setVolumeTags(LogVolId{3}, {{"sensitive", "false"}});
    REQUIRE(tree.cachedCount() == 0);
    auto after = tree.materialize({0, 0});
    REQUIRE(after->tags.at("sensitive") == "false");
    REQUIRE(after->tags.at("layer") == "0");
    REQUIRE(before->tags.at("sensitive") == "true");
    REQUIRE(oldTags.at("sensitive") == "true");
    REQUIRE(tree.tagsFor({1, 0}) == after->tags);
    REQUIRE_THROWS_AS(tree.setVolumeTags(LogVolId{99}, {}), std::out_of_range);
    REQUIRE_THROWS_AS(tree.setPlacementTags(LogVolId{2}, 2, {}), std::out_of_range);
}

TEST_CASE("Sparse display names preserve source paths and sibling identities", "[occurrence]") {
    auto source = prototypes();
    OccurrenceTree tree(source, root(), 1);
    tree.setDisplayName({0, 0}, "ReadoutSensor");
    auto named = tree.materialize({0, 0});
    REQUIRE(named->name == "ReadoutSensor");
    REQUIRE(named->originalPath == "/world/module0/sensor");
    REQUIRE(tree.materialize({0, 1})->name == "sensor");
    tree.clearCache();
    REQUIRE(tree.materialize({0, 0})->name == "ReadoutSensor");
    tree.clearDisplayNameOverride({0, 0});
    REQUIRE(tree.materialize({0, 0})->name == "sensor");
    REQUIRE(named->name == "ReadoutSensor");
}

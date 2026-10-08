#include <catch2/catch_test_macros.hpp>
#include <detail/zstd_io.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <ir/expanded/adapt.hpp>
#include <ir/expanded/conversion.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/legacy/nhs8.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <selection/occurrence_selector.hpp>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::ir::semantic;
using nodehammer::ir::expanded::Node;
using nodehammer::ir::expanded::NodeId;

namespace {
expanded::Scene makeExpandedScene() {
    expanded::Scene s;
    s.rootId = NodeId{1};
    s.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{1, 2, 3}});
    s.materials.emplace(MaterialId{1},
                        SourceMaterial{MaterialId{1}, "Silicon", std::nullopt, 2.33});
    s.logVols.emplace(LogVolId{1}, LogicalVolume{LogVolId{1}, "shared", ShapeId{1}, MaterialId{1}});
    s.logVols.emplace(LogVolId{2},
                      LogicalVolume{LogVolId{2}, "unused daughter", ShapeId{1}, MaterialId{1}});
    // Deliberately stale prototype daughters must never be followed.
    s.logVols.at(LogVolId{1}).daughters.push_back({"removed", LogVolId{2}, glm::dmat4{1}});
    for (uint64_t i = 1; i <= 5; ++i) {
        Node n;
        n.id = NodeId{i};
        n.name = i == 1 ? "world" : "duplicate";
        n.logVolId = LogVolId{1};
        n.localTransform = glm::translate(glm::dmat4{1}, glm::dvec3{double(i), 0, 0});
        if (i != 1)
            n.parentId = NodeId{i <= 3 ? uint64_t{1} : uint64_t{3}};
        s.nodes.emplace(n.id, std::move(n));
    }
    s.nodes.at(NodeId{1}).children = {NodeId{3}, NodeId{2}};
    s.nodes.at(NodeId{3}).children = {NodeId{5}, NodeId{4}};
    s.nodes.at(NodeId{3}).localTransform *=
        glm::rotate(glm::dmat4{1}, glm::radians(45.0), glm::dvec3{0, 0, 1});
    s.computeWorldTransforms();
    s.computeOriginalPaths();
    auto &hoisted = s.nodes.at(NodeId{5});
    hoisted.originalPath = "/world/deleted/old_sensor";
    hoisted.tags = {{"sensitive", "true"}};
    hoisted.sourceSystem = "dd4hep";
    hoisted.degradation.set(DegradationBit::TransformApprox);
    return s;
}
void compare(const expanded::Scene &s, const semantic::Scene &g) {
    auto tree = g.makeTree(1);
    selection::SelectedOccurrences all{*tree};
    REQUIRE(all.size() == s.nodes.size());
    struct Pending {
        NodeId node;
        semantic::OccurrenceId id;
    };
    std::vector<Pending> stack{{s.rootId, {}}};
    while (!stack.empty()) {
        auto p = std::move(stack.back());
        stack.pop_back();
        const auto &n = s.nodes.at(p.node);
        const auto actual = all.materialize(p.id).source;
        CHECK(actual->name == n.name);
        CHECK(actual->originalPath == n.originalPath);
        CHECK(actual->localTransform == n.localTransform);
        CHECK(actual->worldTransform == n.worldTransform);
        CHECK(actual->tags == n.tags);
        CHECK(actual->sourceSystem == n.sourceSystem);
        CHECK(actual->degradation.bits == n.degradation.bits);
        CHECK(actual->childCount == n.children.size());
        for (std::size_t i = 0; i < n.children.size(); ++i) {
            auto id = p.id;
            id.push_back(i);
            stack.push_back({n.children[i], std::move(id)});
        }
    }
}
} // namespace
TEST_CASE("Legacy upgrade keeps actual topology and occurrence metadata", "[ir][shared][legacy]") {
    const auto s = makeExpandedScene();
    const auto g = semantic::fromExpanded(s);
    REQUIRE(g.validate() == 5);

    REQUIRE(g.logVols.size() == 5);
    compare(s, g);
    auto tree = g.makeTree(0);
    selection::SelectedOccurrences all{*tree};
    config::SelectionRule rule;
    rule.action = config::SelectionAction::DropIf;
    rule.predicate = config::PredicateExpr{config::PathGlobPredicate{"/world/deleted/**"}};
    auto selected = all.prune({rule});
    REQUIRE(selected.size() == 4);
    REQUIRE_FALSE(selected.contains({0, 0}));
}
TEST_CASE("Legacy upgrade refuses malformed topology instead of dropping data",
          "[ir][shared][legacy]") {
    auto s = makeExpandedScene();
    SECTION("disconnected") { s.nodes.at(NodeId{3}).children.pop_back(); }
    SECTION("duplicate child") { s.nodes.at(NodeId{3}).children.push_back(NodeId{4}); }
    SECTION("parent disagreement") { s.nodes.at(NodeId{5}).parentId = NodeId{1}; }
    SECTION("missing child") { s.nodes.at(NodeId{3}).children.push_back(NodeId{999}); }
    SECTION("volume ID mismatch") { s.logVols.at(LogVolId{1}).id = LogVolId{123}; }
    SECTION("invalid root") { s.rootId = NodeId{999}; }
    REQUIRE_THROWS(semantic::fromExpanded(s));
}
TEST_CASE("Frozen NHS8 detector fixture upgrades without changing stored occurrences",
          "[ir][shared][legacy]") {
    auto imported = FlatBufferImporter{}.import(std::filesystem::path{NODEHAMMER_FIXTURES_DIR} /
                                                "nhb/legacy-nhs8-mdi.nhb.zst");
    REQUIRE(imported.scene.nodeCount() == 99);
    auto legacy = legacy::nhs8::semanticSceneFromBytes(detail::zstd_io::readBytesFromFile(
        std::filesystem::path{NODEHAMMER_FIXTURES_DIR} / "nhb/legacy-nhs8-mdi.nhb.zst"));
    legacy.computeWorldTransforms();
    legacy.computeOriginalPaths();
    compare(legacy, imported.scene);
}

TEST_CASE("Legacy NHS8 bytes adapt with all stored nodes selected", "[ir][shared][legacy]") {
    const auto bytes = nodehammer::ir::legacy::nhs8::semanticSceneToBytes(makeExpandedScene());
    const auto decoded = FlatBufferImporter::importFromBytes("stored.nhb", bytes);
    REQUIRE(decoded.scene.nodeCount() == 5);
    const auto physical = semantic::expand(decoded.scene);
    std::size_t sensitive = 0;
    for (const auto &[id, node] : physical.nodes) {
        (void)id;
        if (node.tags.contains("sensitive")) {
            ++sensitive;
            REQUIRE(node.sourceSystem == "dd4hep");
            REQUIRE(node.degradation.has(DegradationBit::TransformApprox));
        }
    }
    REQUIRE(sensitive == 1);
    auto stored = legacy::nhs8::semanticSceneFromBytes(bytes);
    stored.computeWorldTransforms();
    stored.computeOriginalPaths();
    compare(stored, decoded.scene);
}

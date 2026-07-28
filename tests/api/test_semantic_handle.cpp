#include <catch2/catch_test_macros.hpp>

#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/scene.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace nodehammer;

namespace {

/// root ── a ── a1
///      │    └─ a2
///      └─ b            (b is a leaf; a2 carries tags)
detail::SemanticScene makeScene() {
    detail::SemanticScene scene;

    const auto mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "silicon", glm::vec3{0.2f, 0.3f, 0.4f}, 2.33};

    const auto boxShape = scene.nextShapeId();
    scene.shapes[boxShape] = {boxShape, BoxShape{1, 2, 3}};
    const auto tubeShape = scene.nextShapeId();
    scene.shapes[tubeShape] = {tubeShape, TubeShape{}};
    const auto boolShape = scene.nextShapeId();
    scene.shapes[boolShape] = {boolShape, BooleanSubtraction{boxShape, tubeShape, glm::dmat4{1.0}}};

    const auto lv = scene.nextLogVolId();
    scene.logVols[lv] = {lv, "boxLV", boxShape, mat};

    auto addNode = [&](const char *name, std::optional<SemanticNodeId> parent) {
        const auto id = scene.nextNodeId();
        SemanticNode n;
        n.id = id;
        n.name = name;
        n.logVolId = lv;
        n.parentId = parent;
        scene.nodes[id] = n;
        if (parent) {
            scene.nodes[*parent].children.push_back(id);
        }
        return id;
    };

    const auto root = addNode("root", std::nullopt);
    scene.rootId = root;
    const auto a = addNode("a", root);
    addNode("a1", a);
    const auto a2 = addNode("a2", a);
    addNode("b", root);

    scene.nodes[a2].tags["subdetector"] = "tracker";
    scene.nodes[a2].tags["sensitive"] = "true";

    scene.computeOriginalPaths();
    return scene;
}

std::vector<std::string> traversalNames(const SemanticScene &scene) {
    std::vector<std::string> names;
    scene.traverse([&](const SemanticScene::Visit &v) {
        names.emplace_back(v.node.name());
        return true;
    });
    return names;
}

} // namespace

TEST_CASE("SemanticScene handle: counts and stats", "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());

    REQUIRE(handle.valid());
    REQUIRE(handle.nodeCount() == 5);
    REQUIRE(handle.logicalVolumeCount() == 1);
    REQUIRE(handle.shapeCount() == 3);
    REQUIRE(handle.materialCount() == 1);

    const auto &stats = handle.stats();
    REQUIRE(stats.reachableNodeCount == 5);
    REQUIRE(stats.leafCount == 3); // a1, a2, b
    REQUIRE(stats.maxDepth == 2);
    REQUIRE(stats.booleanShapeCount == 1);
}

TEST_CASE("SemanticScene handle: a default handle is inert", "[api][semantic]") {
    const SemanticScene handle;

    REQUIRE_FALSE(handle.valid());
    REQUIRE(handle.nodeCount() == 0);
    REQUIRE(handle.nodeIds().empty());
    REQUIRE_FALSE(handle.root().has_value());
    REQUIRE_FALSE(handle.node(SemanticNodeId{1}).has_value());
    REQUIRE(handle.stats().nodeCount == 0);
    REQUIRE(traversalNames(handle).empty()); // must not trap
}

TEST_CASE("SemanticScene handle: traversal is preorder, children in stored order",
          "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());

    REQUIRE(traversalNames(handle) == std::vector<std::string>{"root", "a", "a1", "a2", "b"});

    // nodeIds is the same order, and is what a consumer iterates instead of the
    // backing map.
    std::vector<std::string> viaIds;
    for (const auto id : handle.nodeIds()) {
        viaIds.emplace_back(handle.node(id)->name());
    }
    REQUIRE(viaIds == traversalNames(handle));
}

TEST_CASE("SemanticScene handle: traversal carries depth and sibling position", "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());

    handle.traverse([](const SemanticScene::Visit &v) {
        if (v.node.name() == "root") {
            REQUIRE(v.depth == 0);
            REQUIRE(v.isLastSibling);
        } else if (v.node.name() == "a") {
            REQUIRE(v.depth == 1);
            REQUIRE(v.siblingIndex == 0);
            REQUIRE(v.siblingCount == 2);
            REQUIRE_FALSE(v.isLastSibling);
        } else if (v.node.name() == "b") {
            REQUIRE(v.depth == 1);
            REQUIRE(v.siblingIndex == 1);
            REQUIRE(v.isLastSibling);
        } else if (v.node.name() == "a2") {
            REQUIRE(v.depth == 2);
            REQUIRE(v.isLastSibling);
        }
        return true;
    });
}

TEST_CASE("SemanticScene handle: returning false prunes the subtree", "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());

    std::vector<std::string> names;
    handle.traverse([&](const SemanticScene::Visit &v) {
        names.emplace_back(v.node.name());
        return v.node.name() != "a"; // skip a's children
    });

    REQUIRE(names == std::vector<std::string>{"root", "a", "b"});
}

TEST_CASE("SemanticScene handle: traversal survives a cycle", "[api][semantic]") {
    auto raw = makeScene();
    // Point a leaf back at the root. visitBFS used to spin forever on this.
    const auto rootId = raw.rootId;
    for (auto &[id, node] : raw.nodes) {
        if (node.name == "b") {
            node.children.push_back(rootId);
        }
    }
    const auto handle = wrapSemanticScene(std::move(raw));

    const auto names = traversalNames(handle);
    REQUIRE(names.size() == 5); // each node once, no repeat of root
}

TEST_CASE("SemanticScene handle: traversal skips a dangling child id", "[api][semantic]") {
    auto raw = makeScene();
    raw.nodes[raw.rootId].children.push_back(SemanticNodeId{9999});
    const auto handle = wrapSemanticScene(std::move(raw));

    REQUIRE_NOTHROW(traversalNames(handle));
    REQUIRE(traversalNames(handle).size() == 5);
}

TEST_CASE("SemanticScene handle: node -> logVol -> {shape, material} chain", "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());
    const auto root = handle.root();
    REQUIRE(root.has_value());

    REQUIRE(root->logicalVolume().has_value());
    REQUIRE(root->logicalVolume()->name() == "boxLV");
    REQUIRE(root->shape().has_value());
    REQUIRE(root->shape()->kind() == ShapeKind::Box);
    REQUIRE(root->shape()->kindName() == "box");
    REQUIRE(root->material().has_value());
    REQUIRE(root->material()->name() == "silicon");
    REQUIRE(root->material()->density() == 2.33);
    REQUIRE(root->material()->color().has_value());
}

TEST_CASE("SemanticScene handle: shape kinds map to the variant alternatives", "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());

    std::vector<ShapeKind> kinds;
    for (const auto id : handle.shapeIds()) {
        kinds.push_back(handle.shape(id)->kind());
    }
    REQUIRE(kinds ==
            std::vector<ShapeKind>{ShapeKind::Box, ShapeKind::Tube, ShapeKind::Subtraction});

    const auto boolShape = handle.shape(handle.shapeIds()[2]);
    REQUIRE(boolShape->isBoolean());
    REQUIRE(boolShape->booleanLeft().has_value());
    REQUIRE(boolShape->booleanRight().has_value());
    REQUIRE(*boolShape->booleanLeft() == handle.shapeIds()[0]);

    REQUIRE_FALSE(handle.shape(handle.shapeIds()[0])->isBoolean());
    REQUIRE_FALSE(handle.shape(handle.shapeIds()[0])->booleanLeft().has_value());
}

TEST_CASE("SemanticScene handle: every ShapeKind has a distinct name", "[api][semantic]") {
    std::vector<std::string_view> names;
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(ShapeKind::Unknown); ++i) {
        names.push_back(shapeKindName(static_cast<ShapeKind>(i)));
    }
    auto unique = names;
    std::sort(unique.begin(), unique.end());
    REQUIRE(std::adjacent_find(unique.begin(), unique.end()) == unique.end());
    REQUIRE(names.size() == 13);
}

TEST_CASE("SemanticScene handle: tags are accessible without exposing the map", "[api][semantic]") {
    const auto handle = wrapSemanticScene(makeScene());

    const SemanticNodeView *tagged = nullptr;
    std::optional<SemanticNodeView> holder;
    handle.traverse([&](const SemanticScene::Visit &v) {
        if (v.node.name() == "a2") {
            holder = v.node;
        }
        return true;
    });
    REQUIRE(holder.has_value());
    tagged = &*holder;

    REQUIRE(tagged->tagCount() == 2);
    REQUIRE(tagged->tag("subdetector") == "tracker");
    REQUIRE(tagged->tag("sensitive") == "true");
    REQUIRE_FALSE(tagged->tag("absent").has_value());

    std::vector<std::string> keys;
    tagged->forEachTag([&](std::string_view k, std::string_view) { keys.emplace_back(k); });
    REQUIRE(keys == std::vector<std::string>{"sensitive", "subdetector"}); // key order
}

TEST_CASE("SemanticScene handle: a view outlives the handle it came from", "[api][semantic]") {
    std::optional<SemanticNodeView> view;
    {
        const auto handle = wrapSemanticScene(makeScene());
        view = handle.root();
        REQUIRE(view.has_value());
    }
    REQUIRE(view->name() == "root");
    REQUIRE(view->originalPath() == "/root");
    REQUIRE(view->childCount() == 2);
    REQUIRE(view->scene().valid());
}

TEST_CASE("SemanticScene handle: unreachable nodes count but are not traversed",
          "[api][semantic]") {
    auto raw = makeScene();
    // Detached node, as a partial prune can leave behind.
    const auto orphan = raw.nextNodeId();
    SemanticNode n;
    n.id = orphan;
    n.name = "orphan";
    raw.nodes[orphan] = n;

    const auto handle = wrapSemanticScene(std::move(raw));

    REQUIRE(handle.stats().nodeCount == 6);
    REQUIRE(handle.stats().reachableNodeCount == 5);
    REQUIRE(handle.nodeIds().size() == 5);
    // Still directly addressable — it is in the map, just not in the tree.
    REQUIRE(handle.node(orphan).has_value());
}

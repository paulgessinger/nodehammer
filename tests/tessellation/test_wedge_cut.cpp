#include <catch2/catch_test_macros.hpp>

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using namespace nodehammer;

namespace {

// A flat scene: a large root "world" box at the origin plus four leaf boxes.
// Three leaves (A, B, C) share one logical volume so we can check instancing;
// D is a separate, deliberately straddling box.
//
// Removed sector used by the tests is [0°, 90°] (the +x/+y quadrant).
//   A  @ (-10,-10)  → kept     (third quadrant)
//   B  @ (-12, -8)  → kept     (third quadrant, same logVol as A → instanced)
//   C  @ ( 10, 10)  → emptied  (inside removed quadrant)
//   D  @ ( 10,  0)  → straddle (crosses the +x / 0° boundary)
struct TestScene {
    SemanticScene scene;
    SemanticNodeId a, b, c, d, root;
};

TestScene makeScene() {
    TestScene ts;
    SemanticScene &scene = ts.scene;

    const SemanticMaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};

    // Shared leaf box (half-extents 1) and the straddling box (wider in x).
    const SemanticShapeId boxShape = scene.nextShapeId();
    scene.shapes[boxShape] = {boxShape, BoxShape{1, 1, 1}};
    const SemanticShapeId wideShape = scene.nextShapeId();
    scene.shapes[wideShape] = {wideShape, BoxShape{2, 1, 1}};
    const SemanticShapeId worldShape = scene.nextShapeId();
    scene.shapes[worldShape] = {worldShape, BoxShape{20, 20, 20}};

    const SemanticLogVolId boxLv = scene.nextLogVolId();
    scene.logVols[boxLv] = {boxLv, "box_lv", boxShape, mat};
    const SemanticLogVolId wideLv = scene.nextLogVolId();
    scene.logVols[wideLv] = {wideLv, "wide_lv", wideShape, mat};
    const SemanticLogVolId worldLv = scene.nextLogVolId();
    scene.logVols[worldLv] = {worldLv, "world_lv", worldShape, mat};

    auto addNode = [&](const char *name, SemanticLogVolId lv, glm::dvec3 pos) {
        const SemanticNodeId id = scene.nextNodeId();
        SemanticNode n;
        n.id = id;
        n.name = name;
        n.logVolId = lv;
        n.localTransform = glm::translate(glm::dmat4{1.0}, pos);
        scene.nodes[id] = std::move(n);
        return id;
    };

    ts.root = addNode("world", worldLv, {0, 0, 0});
    scene.rootId = ts.root;
    scene.nodes[ts.root].localTransform = glm::dmat4{1.0};

    ts.a = addNode("A", boxLv, {-10, -10, 0});
    ts.b = addNode("B", boxLv, {-12, -8, 0});
    ts.c = addNode("C", boxLv, {10, 10, 0});
    ts.d = addNode("D", wideLv, {10, 0, 0});

    auto &root = scene.nodes[ts.root];
    root.children = {ts.a, ts.b, ts.c, ts.d};
    for (auto child : root.children) {
        scene.nodes[child].parentId = ts.root;
    }

    scene.computeWorldTransforms();
    return ts;
}

const detail::RenderNode *findBySemId(const detail::RenderScene &rs, SemanticNodeId sid) {
    for (const auto &[id, rn] : rs.nodes) {
        (void)id;
        if (rn.semanticNodeId == sid) {
            return &rn;
        }
    }
    return nullptr;
}

bool meshNonEmpty(const detail::RenderScene &rs, MeshAssetId mid) {
    auto it = rs.meshAssets.find(mid);
    return it != rs.meshAssets.end() && !it->second.indices.empty();
}

} // namespace

TEST_CASE("wedge cut: classifies placements by sector", "[tessellation][wedgecut]") {
    auto ts = makeScene();
    const auto stats = applyWedgeCut(ts.scene, {0.0, 90.0});

    // A, B kept; C emptied; D + world straddle.
    CHECK(stats.kept == 2);
    CHECK(stats.emptied == 1);
    CHECK(stats.cut == 2);
    // world and D are different shapes → two distinct cut meshes.
    CHECK(stats.cutUnique == 2);
    CHECK(stats.skipped == 0);
    // C is a leaf fully inside the removed sector under a root that keeps
    // geometry → it forms a maximal empty subtree and is pruned from the scene.
    CHECK(stats.pruned == 1);
    CHECK_FALSE(ts.scene.nodes.contains(ts.c));
}

TEST_CASE("wedge cut: lowers to a valid render scene", "[tessellation][wedgecut]") {
    auto ts = makeScene();
    (void)applyWedgeCut(ts.scene, {0.0, 90.0});

    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(ts.scene);
    REQUIRE_FALSE(result.diags.hasErrors());

    const detail::RenderNode *a = findBySemId(result.scene, ts.a);
    const detail::RenderNode *b = findBySemId(result.scene, ts.b);
    const detail::RenderNode *d = findBySemId(result.scene, ts.d);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(d);

    // Kept placements keep a (shared) mesh — instancing preserved.
    REQUIRE(a->meshBindings.size() == 1);
    REQUIRE(b->meshBindings.size() == 1);
    CHECK(a->meshBindings[0].meshId == b->meshBindings[0].meshId);
    CHECK(meshNonEmpty(result.scene, a->meshBindings[0].meshId));

    // Fully-inside placement is pruned from the scene entirely.
    CHECK(findBySemId(result.scene, ts.c) == nullptr);

    // Straddling placement gets its own non-empty cut mesh, distinct from the
    // shared kept mesh.
    REQUIRE(d->meshBindings.size() == 1);
    CHECK(meshNonEmpty(result.scene, d->meshBindings[0].meshId));
    CHECK(d->meshBindings[0].meshId != a->meshBindings[0].meshId);
}

TEST_CASE("wedge cut: removes the geometry on the requested side", "[tessellation][wedgecut]") {
    // Regression guard for the cut *orientation*. D is a wide box at world
    // (10,0,0) spanning x∈[8,12], y∈[-1,1]. Removing the [0°,90°] sector takes
    // the +y half (world φ∈(0,90°)) and keeps the -y half (φ∈(270°,360°)). If the
    // cutting wedge were built on the opposite side (a 180° error), the box would
    // not intersect it and survive intact — so its mesh would still carry +y
    // vertices. The box is a pure translation, so its local mesh y matches world.
    auto ts = makeScene();
    (void)applyWedgeCut(ts.scene, {0.0, 90.0});

    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(ts.scene);
    REQUIRE_FALSE(result.diags.hasErrors());

    const detail::RenderNode *d = findBySemId(result.scene, ts.d);
    REQUIRE(d);
    REQUIRE(d->meshBindings.size() == 1);
    const auto meshIt = result.scene.meshAssets.find(d->meshBindings[0].meshId);
    REQUIRE(meshIt != result.scene.meshAssets.end());
    const auto &verts = meshIt->second.vertices;
    REQUIRE_FALSE(verts.empty());

    float maxY = -1e9f, minY = 1e9f;
    for (const auto &v : verts) {
        maxY = std::max(maxY, v.position.y);
        minY = std::min(minY, v.position.y);
    }
    // The +y half is gone (cut plane at y≈0); the -y half is retained.
    CHECK(maxY < 1e-3f);
    CHECK(minY < -0.9f);
}

TEST_CASE("wedge cut: a narrow sector inside a wide AABB still cuts", "[tessellation][wedgecut]") {
    // Regression for the ODD muon-endcap-wedge failure, in miniature. A wide box
    // whose world AABB subtends a large angle is cut by a *narrow* sector that
    // lies between the AABB's corner angles. The old corner-membership test saw
    // no corner inside the removed sector and wrongly classified the box as
    // fully kept (so it was never cut); the angular-arc test classifies it as
    // straddling, as it must.
    SemanticScene scene;
    const SemanticMaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};
    // World AABB x∈[9,11], y∈[-8,8] → angular span ≈ ±42°, corners near ±36/±42°,
    // none in the [10°,20°] sector we remove (which the box nonetheless covers).
    const SemanticShapeId box = scene.nextShapeId();
    scene.shapes[box] = {box, BoxShape{1, 8, 1}};
    const SemanticLogVolId lv = scene.nextLogVolId();
    scene.logVols[lv] = {lv, "wide", box, mat};

    const SemanticNodeId root = scene.nextNodeId();
    SemanticNode n;
    n.id = root;
    n.name = "wide";
    n.logVolId = lv;
    n.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{10, 0, 0});
    scene.nodes[root] = std::move(n);
    scene.rootId = root;
    scene.computeWorldTransforms();

    const auto stats = applyWedgeCut(scene, {10.0, 20.0});
    CHECK(stats.cut == 1); // straddles → boolean-cut
    CHECK(stats.kept == 0);
    CHECK(stats.emptied == 0);
}

TEST_CASE("wedge cut: degenerate sectors are a no-op", "[tessellation][wedgecut]") {

    auto same = makeScene();
    const auto shapesBefore = same.scene.shapes.size();
    const auto statsSame = applyWedgeCut(same.scene, {45.0, 45.0});
    CHECK(statsSame.cut == 0);
    CHECK(statsSame.emptied == 0);
    CHECK(statsSame.kept == 0);
    CHECK(same.scene.shapes.size() == shapesBefore);

    auto full = makeScene();
    const auto statsFull = applyWedgeCut(full.scene, {0.0, 360.0});
    CHECK(statsFull.cut == 0);
    CHECK(full.scene.shapes.size() == shapesBefore);
}

TEST_CASE("wedge cut: wrap-around sector across 0 degrees", "[tessellation][wedgecut]") {
    auto ts = makeScene();
    // Removed 10° straddling the +x axis (355°→5°), narrower than D's ~14°
    // angular span so D crosses both boundaries → straddle; A and B (third
    // quadrant) are well clear → kept.
    const auto stats = applyWedgeCut(ts.scene, {355.0, 5.0});
    CHECK(stats.kept >= 2);
    CHECK(stats.cut >= 1);

    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(ts.scene);
    CHECK_FALSE(result.diags.hasErrors());
    const detail::RenderNode *d = findBySemId(result.scene, ts.d);
    REQUIRE(d);
    REQUIRE(d->meshBindings.size() == 1);
    CHECK(meshNonEmpty(result.scene, d->meshBindings[0].meshId));
}

TEST_CASE("wedge cut: a fully-inside subtree is pruned wholesale", "[tessellation][wedgecut]") {
    // Mimics a stave (envelope) with child modules, the whole assembly sitting
    // inside the removed sector. The envelope + all children must be pruned so a
    // merge_descendants parent is never asked to merge an empty child set.
    SemanticScene scene;
    const SemanticMaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};
    const SemanticShapeId box = scene.nextShapeId();
    scene.shapes[box] = {box, BoxShape{1, 1, 1}};
    const SemanticLogVolId lv = scene.nextLogVolId();
    scene.logVols[lv] = {lv, "lv", box, mat};

    auto add = [&](const char *name, glm::dvec3 pos, std::optional<SemanticNodeId> parent) {
        const SemanticNodeId id = scene.nextNodeId();
        SemanticNode n;
        n.id = id;
        n.name = name;
        n.logVolId = lv;
        n.localTransform = glm::translate(glm::dmat4{1.0}, pos);
        n.parentId = parent;
        scene.nodes[id] = n;
        return id;
    };
    // Root box at the origin straddles → keeps geometry (and as the root is never
    // pruned). The stave envelope + its two modules sit fully inside the removed
    // quadrant (absolute positions; root is at the origin so no inherited offset).
    const SemanticNodeId root = add("root", {0, 0, 0}, std::nullopt);
    scene.rootId = root;
    scene.nodes[root].localTransform = glm::dmat4{1.0};
    const SemanticNodeId stave = add("stave", {30, 30, 0}, root);
    const SemanticNodeId m0 = add("mod0", {31, 30, 0}, stave);
    const SemanticNodeId m1 = add("mod1", {29, 30, 0}, stave);
    scene.nodes[root].children = {stave};
    scene.nodes[stave].children = {m0, m1};
    // mod positions are relative to the stave at (30,30); offset back so their
    // world positions are (31,30) and (29,30).
    scene.nodes[m0].localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{1, 0, 0});
    scene.nodes[m1].localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{-1, 0, 0});
    scene.computeWorldTransforms();

    const auto stats = applyWedgeCut(scene, {0.0, 90.0});
    CHECK(stats.cut == 1);     // the root straddles
    CHECK(stats.emptied == 3); // stave + 2 modules fully inside
    CHECK(stats.pruned == 3);  // and all three pruned
    CHECK(scene.nodes.contains(root));
    CHECK_FALSE(scene.nodes.contains(stave));
    CHECK_FALSE(scene.nodes.contains(m0));
    CHECK_FALSE(scene.nodes.contains(m1));
    // Root's child list no longer references the pruned stave.
    CHECK(scene.nodes.at(root).children.empty());
}

TEST_CASE("wedge cut: instances sharing a local-frame cut stay instanced",
          "[tessellation][wedgecut]") {
    // Three boxes of the same shape, all straddling. Two sit on the +x axis and
    // differ only by a translation along the cut (z) axis → identical local-frame
    // wedge → must share one cut mesh. The third sits on the +y axis (different
    // phi) → distinct cut. So: 3 cut placements, 2 unique cut meshes.
    SemanticScene scene;
    const SemanticMaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};
    const SemanticShapeId shape = scene.nextShapeId();
    scene.shapes[shape] = {shape, BoxShape{2, 1, 1}};
    const SemanticLogVolId lv = scene.nextLogVolId();
    scene.logVols[lv] = {lv, "lv", shape, mat};

    // A non-cuttable empty root at the origin so children sit at absolute world
    // positions (no inherited translation to reason about).
    const SemanticShapeId emptyShape = scene.nextShapeId();
    scene.shapes[emptyShape] = {emptyShape, TessellatedShape{}};
    const SemanticLogVolId emptyLv = scene.nextLogVolId();
    scene.logVols[emptyLv] = {emptyLv, "root_lv", emptyShape, mat};

    auto add = [&](const char *name, SemanticLogVolId vol, glm::dvec3 pos,
                   std::optional<SemanticNodeId> parent) {
        const SemanticNodeId id = scene.nextNodeId();
        SemanticNode n;
        n.id = id;
        n.name = name;
        n.logVolId = vol;
        n.localTransform = glm::translate(glm::dmat4{1.0}, pos);
        n.parentId = parent;
        scene.nodes[id] = n;
        return id;
    };
    const SemanticNodeId root = add("root", emptyLv, {0, 0, 0}, std::nullopt);
    scene.rootId = root;
    // Two copies on +x differing only along z (shared cut); one on +y (distinct).
    const SemanticNodeId pxZ0 = add("px_z0", lv, {10, 0, 0}, root);
    const SemanticNodeId pxZ50 = add("px_z50", lv, {10, 0, 50}, root);
    const SemanticNodeId py = add("py_z0", lv, {0, 10, 0}, root);
    scene.nodes[root].children = {pxZ0, pxZ50, py};
    scene.computeWorldTransforms();

    const auto stats = applyWedgeCut(scene, {0.0, 90.0});
    CHECK(stats.cut == 3);
    CHECK(stats.cutUnique == 2);

    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(scene);
    REQUIRE_FALSE(result.diags.hasErrors());

    const detail::RenderNode *r0 = findBySemId(result.scene, pxZ0);
    const detail::RenderNode *rz = findBySemId(result.scene, pxZ50);
    const detail::RenderNode *rpy = findBySemId(result.scene, py);
    REQUIRE(r0);
    REQUIRE(rz);
    REQUIRE(rpy);
    REQUIRE(r0->meshBindings.size() == 1);
    REQUIRE(rz->meshBindings.size() == 1);
    REQUIRE(rpy->meshBindings.size() == 1);
    // The two +x copies share a cut mesh; the +y copy is distinct.
    CHECK(r0->meshBindings[0].meshId == rz->meshBindings[0].meshId);
    CHECK(r0->meshBindings[0].meshId != rpy->meshBindings[0].meshId);
}

TEST_CASE("wedge cut: allocates fresh IDs without overwriting existing shapes",
          "[tessellation][wedgecut]") {
    // Simulate a deserialized scene whose ID counters are stale (start at 1)
    // while existing entries use large IDs.
    SemanticScene scene;
    const SemanticMaterialId mat{500};
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};
    const SemanticShapeId shape{1000};
    scene.shapes[shape] = {shape, BoxShape{2, 1, 1}};
    const SemanticLogVolId lv{2000};
    scene.logVols[lv] = {lv, "lv", shape, mat};
    const SemanticNodeId node{3000};
    SemanticNode n;
    n.id = node;
    n.name = "straddle";
    n.logVolId = lv;
    n.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{10, 0, 0});
    scene.nodes[node] = n;
    scene.rootId = node;
    scene.computeWorldTransforms();

    const auto stats = applyWedgeCut(scene, {0.0, 90.0});
    CHECK(stats.cut == 1);
    // The original box shape must still be intact (not overwritten by a new
    // wedge/cut shape that collided with ID 1000…).
    REQUIRE(scene.shapes.contains(shape));
    CHECK(std::holds_alternative<BoxShape>(scene.shapes.at(shape).data));
}

TEST_CASE("wedge cut: an emptied Boolean inside a merge group is not a failure",
          "[tessellation][wedgecut]") {
    // Regression for the ODD `slice1_0` failure. A merge_descendants parent with
    // fallback="fail" contains a descendant whose Boolean cut removes it entirely
    // (the solid lay wholly inside the removed wedge, or its kept sliver collapsed
    // below Manifold's tolerance). That yields an empty *but succeeded* mesh. The
    // merge path used to treat empty-vertices as a tessellation failure and emit
    // NH0503; it must instead skip the emptied descendant and still merge the rest.
    SemanticScene scene;

    const auto mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};

    // A small box fully contained inside a larger subtractor at the same place →
    // Box - Box is empty, and Manifold reports success (NoError).
    const auto smallShape = scene.nextShapeId();
    scene.shapes[smallShape] = {smallShape, BoxShape{1, 1, 1}};
    const auto bigShape = scene.nextShapeId();
    scene.shapes[bigShape] = {bigShape, BoxShape{5, 5, 5}};
    const auto emptiedShape = scene.nextShapeId();
    scene.shapes[emptiedShape] = {emptiedShape,
                                  BooleanSubtraction{smallShape, bigShape, glm::dmat4{1.0}}};

    // An ordinary box sibling so the merge group still produces geometry.
    const auto keepShape = scene.nextShapeId();
    scene.shapes[keepShape] = {keepShape, BoxShape{1, 1, 1}};
    const auto worldShape = scene.nextShapeId();
    scene.shapes[worldShape] = {worldShape, BoxShape{20, 20, 20}};

    const auto emptiedLv = scene.nextLogVolId();
    scene.logVols[emptiedLv] = {emptiedLv, "emptied_lv", emptiedShape, mat};
    const auto keepLv = scene.nextLogVolId();
    scene.logVols[keepLv] = {keepLv, "keep_lv", keepShape, mat};
    const auto staveLv = scene.nextLogVolId();
    scene.logVols[staveLv] = {staveLv, "stave_lv", worldShape, mat};

    auto addNode = [&](const char *name, SemanticLogVolId lv, std::optional<SemanticNodeId> parent,
                       glm::dvec3 pos) {
        const auto id = scene.nextNodeId();
        SemanticNode n;
        n.id = id;
        n.name = name;
        n.logVolId = lv;
        n.localTransform = glm::translate(glm::dmat4{1.0}, pos);
        n.parentId = parent;
        scene.nodes[id] = n;
        return id;
    };

    const auto stave = addNode("stave1", staveLv, std::nullopt, {0, 0, 0});
    scene.rootId = stave;
    const auto emptied = addNode("emptied_slice", emptiedLv, stave, {0, 0, 0});
    const auto kept = addNode("kept_slice", keepLv, stave, {3, 0, 0});
    scene.nodes[stave].children = {emptied, kept};
    scene.computeWorldTransforms();

    // Merge the stave's descendants with the strictest fallback.
    NHConfig cfg;
    Rule mergeRule;
    mergeRule.match = PredicateExpr{NameGlobPredicate{"stave*"}};
    mergeRule.tessellation = Rule::Tessellation{};
    mergeRule.tessellation->mergeDescendants = true;
    mergeRule.tessellation->fallback = BooleanFallback::Fail;
    cfg.rules.push_back(mergeRule);

    TessellationPass pass{cfg};
    auto result = pass.lower(scene);

    // The emptied Boolean must not be reported as a failure …
    CHECK_FALSE(result.diags.hasErrors());
    // … and the surviving sibling must still contribute a merged mesh.
    const detail::RenderNode *rn = findBySemId(result.scene, stave);
    REQUIRE(rn);
    REQUIRE(rn->meshBindings.size() == 1);
    CHECK(meshNonEmpty(result.scene, rn->meshBindings[0].meshId));
}

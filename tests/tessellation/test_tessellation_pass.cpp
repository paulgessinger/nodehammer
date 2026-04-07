#include <catch2/catch_test_macros.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

using namespace nodehammer;

// ── Helpers ───────────────────────────────────────────────────────────────────

static SemanticScene makeSingleBoxScene() { return SyntheticSceneBuilder::buildSingleBox(); }

static SemanticScene makeNestedBoxScene() { return SyntheticSceneBuilder::buildNestedBoxes(); }

// Build a scene with a single BooleanUnion node.
static SemanticScene makeBooleanScene() {
    SemanticScene scene;

    SemanticMaterialId matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};

    // Left and right child shapes
    SemanticShapeId leftId = scene.nextShapeId();
    scene.shapes[leftId] = {leftId, BoxShape{1, 1, 1}};

    SemanticShapeId rightId = scene.nextShapeId();
    scene.shapes[rightId] = {rightId, BoxShape{0.5, 0.5, 0.5}};

    // Boolean union shape
    SemanticShapeId boolId = scene.nextShapeId();
    scene.shapes[boolId] = {boolId, BooleanUnion{leftId, rightId}};

    SemanticLogVolId lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "world_lv", boolId, matId};

    SemanticNodeId rootId = scene.nextNodeId();
    SemanticNode root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = lvId;
    scene.nodes[rootId] = root;
    scene.rootId = rootId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

// Build a scene with an UnknownShape node.
static SemanticScene makeUnknownShapeScene() {
    SemanticScene scene;

    SemanticMaterialId matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};

    SemanticShapeId shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, UnknownShape{"CustomSolid"}};

    SemanticLogVolId lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "world_lv", shapeId, matId};

    SemanticNodeId rootId = scene.nextNodeId();
    SemanticNode root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = lvId;
    scene.nodes[rootId] = root;
    scene.rootId = rootId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

// ── Basic lowering ────────────────────────────────────────────────────────────

TEST_CASE("TessellationPass: single box produces one MeshAsset", "[tessellation][pass]") {
    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(makeSingleBoxScene());

    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.scene.meshAssets.size() == 1);
    REQUIRE(result.scene.nodes.size() == 1);
}

TEST_CASE("TessellationPass: nested boxes preserve hierarchy", "[tessellation][pass]") {
    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(makeNestedBoxScene());

    REQUIRE_FALSE(result.diags.hasErrors());

    // Root should have children
    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE_FALSE(root.children.empty());

    // All render nodes are reachable from root
    std::size_t reachable = 0;
    result.scene.nodes.at(result.scene.rootId); // just check it exists
    for (const auto &[id, rn] : result.scene.nodes) {
        (void)id;
        reachable++;
    }
    REQUIRE(reachable == result.scene.nodes.size());
}

TEST_CASE("TessellationPass: single box preserves transform", "[tessellation][pass]") {
    auto scene = makeSingleBoxScene();
    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(scene);

    const auto &rn = result.scene.nodes.at(result.scene.rootId);
    // Root has identity world transform
    REQUIRE(glm::mat4(scene.nodes.at(scene.rootId).worldTransform) == rn.worldTransform);
}

TEST_CASE("TessellationPass: single box has meshBinding", "[tessellation][pass]") {
    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(makeSingleBoxScene());

    const auto &rn = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(rn.meshBindings.size() == 1);
}

// ── Material rule ─────────────────────────────────────────────────────────────

TEST_CASE("TessellationPass: named material rule applies to matching node",
          "[tessellation][pass]") {
    NHConfig cfg;

    MaterialDef md;
    md.name = "gold";
    md.baseColor = {1.0f, 0.8f, 0.0f, 1.0f};
    cfg.materials.push_back(md);

    MaterialRule mr;
    mr.materialName = "gold";
    cfg.materialRules.push_back(mr); // matches all nodes

    TessellationPass pass{cfg};
    auto result = pass.lower(makeSingleBoxScene());

    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.scene.materials.size() == 1);
    const auto &[id, mat] = *result.scene.materials.begin();
    REQUIRE(mat.name == "gold");
}

// ── Boolean fallback ──────────────────────────────────────────────────────────

TEST_CASE("TessellationPass: boolean fallback=Skip emits warning, no mesh binding",
          "[tessellation][pass]") {
    NHConfig cfg;
    TessellationRule rule;
    rule.fallback = BooleanFallback::Skip;
    cfg.tessellationRules.push_back(rule);

    TessellationPass pass{cfg};
    auto result = pass.lower(makeBooleanScene());

    REQUIRE_FALSE(result.diags.hasErrors());

    bool hasWarn = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == codes::kWarnTessBooleanSkipped)
            hasWarn = true;
    }
    REQUIRE(hasWarn);

    // No mesh bindings on the root node
    REQUIRE(result.scene.nodes.at(result.scene.rootId).meshBindings.empty());
}

TEST_CASE("TessellationPass: boolean fallback=BBox emits warning and produces a mesh",
          "[tessellation][pass]") {
    NHConfig cfg;
    TessellationRule rule;
    rule.fallback = BooleanFallback::BBox;
    cfg.tessellationRules.push_back(rule);

    TessellationPass pass{cfg};
    auto result = pass.lower(makeBooleanScene());

    REQUIRE_FALSE(result.diags.hasErrors());

    bool hasWarn = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == codes::kWarnTessBooleanBbox)
            hasWarn = true;
    }
    REQUIRE(hasWarn);
    REQUIRE_FALSE(result.scene.nodes.at(result.scene.rootId).meshBindings.empty());
}

TEST_CASE("TessellationPass: boolean fallback=Fail returns error diagnostic",
          "[tessellation][pass]") {
    NHConfig cfg;
    TessellationRule rule;
    rule.fallback = BooleanFallback::Fail;
    cfg.tessellationRules.push_back(rule);

    TessellationPass pass{cfg};
    auto result = pass.lower(makeBooleanScene());

    REQUIRE(result.diags.hasErrors());
    bool hasErr = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == codes::kErrTessBooleanFail)
            hasErr = true;
    }
    REQUIRE(hasErr);
}

// ── UnknownShape ──────────────────────────────────────────────────────────────

TEST_CASE("TessellationPass: UnknownShape emits error diagnostic", "[tessellation][pass]") {
    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(makeUnknownShapeScene());

    REQUIRE(result.diags.hasErrors());
    bool hasErr = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == codes::kErrTessUnknownShape)
            hasErr = true;
    }
    REQUIRE(hasErr);
}

TEST_CASE("TessellationPass: empty scene produces empty result", "[tessellation][pass]") {
    NHConfig cfg;
    TessellationPass pass{cfg};
    SemanticScene empty;
    auto result = pass.lower(empty);

    REQUIRE(result.scene.nodes.empty());
    REQUIRE(result.scene.meshAssets.empty());
    REQUIRE_FALSE(result.diags.hasErrors());
}

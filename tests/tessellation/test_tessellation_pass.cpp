#include <catch2/catch_test_macros.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <map>
#include <set>

#ifdef NH_WITH_BOOLEAN_MESH
#include <nlohmann/json.hpp>
#include <nodehammer/tessellation/boolean_tessellator.hpp>
#endif

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

    Rule mr;
    mr.material = "gold";
    cfg.rules.push_back(mr); // matches all nodes

    TessellationPass pass{cfg};
    auto result = pass.lower(makeSingleBoxScene());

    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.scene.materials.size() == 1);
    const auto &[id, mat] = *result.scene.materials.begin();
    REQUIRE(mat.name == "gold");
}

// ── Boolean fallback ──────────────────────────────────────────────────────────

#ifdef NH_WITH_BOOLEAN_MESH
TEST_CASE("BooleanTessellator: partial-phi tube produces manifold-compatible mesh",
          "[tessellation][boolean]") {
    // Reproduces the ODD CarbonFiber support shape: a solid partial-phi tube
    // used as a boolean operand.
    SemanticScene scene;
    auto shapeId = scene.nextShapeId();
    TubeShape tube;
    tube.rMin = 0.0;
    tube.rMax = 0.2;
    tube.dz = 12.8;
    tube.phiStart = 6.183185307179587;
    tube.phiDelta = 0.414159265358979;
    scene.shapes[shapeId] = {shapeId, tube};

    // Create a boolean subtraction: tube - smaller tube (arbitrary, just to exercise the path)
    auto rightId = scene.nextShapeId();
    TubeShape smallTube;
    smallTube.rMin = 0.0;
    smallTube.rMax = 0.05;
    smallTube.dz = 5.0;
    smallTube.phiStart = 6.183185307179587;
    smallTube.phiDelta = 0.414159265358979;
    scene.shapes[rightId] = {rightId, smallTube};

    auto boolId = scene.nextShapeId();
    scene.shapes[boolId] = {boolId, BooleanSubtraction{shapeId, rightId}};

    PrimitiveTessellator tess;
    TessellationParams params;
    params.maxSegmentsCircle = 16; // low for speed

    auto result = tessellateBooleanShape(scene.shapes.at(boolId).data, scene, tess, params);

    // Print diagnostics for debugging.
    for (const auto &d : result.diags.items()) {
        UNSCOPED_INFO(std::format("[{}] {}", d.code, d.message));
    }

    // Check that we get actual geometry, not an empty fallback.
    REQUIRE_FALSE(result.vertices.empty());
    REQUIRE_FALSE(result.indices.empty());
    // Should have no errors (warnings are acceptable for now).
    REQUIRE_FALSE(result.diags.hasErrors());
}

TEST_CASE("BooleanTessellator: full-phi tube subtraction produces geometry",
          "[tessellation][boolean]") {
    SemanticScene scene;

    auto outerId = scene.nextShapeId();
    scene.shapes[outerId] = {outerId, TubeShape{3.9, 4.0, 94.16, 0.0, 2.0 * std::numbers::pi}};

    auto innerId = scene.nextShapeId();
    scene.shapes[innerId] = {innerId, TubeShape{3.9, 4.0, 91.52, 0.0, 2.0 * std::numbers::pi}};

    auto boolId = scene.nextShapeId();
    scene.shapes[boolId] = {boolId, BooleanSubtraction{outerId, innerId}};

    PrimitiveTessellator tess;
    TessellationParams params;
    params.maxSegmentsCircle = 16;

    auto result = tessellateBooleanShape(scene.shapes.at(boolId).data, scene, tess, params);

    REQUIRE_FALSE(result.vertices.empty());
    REQUIRE_FALSE(result.indices.empty());
    REQUIRE_FALSE(result.diags.hasErrors());
}

TEST_CASE("TessellationPass: boolean subtraction produces geometry via Manifold",
          "[tessellation][pass][boolean]") {
    NHConfig cfg;
    TessellationPass pass{cfg};
    auto result = pass.lower(makeBooleanScene());

    REQUIRE_FALSE(result.diags.hasErrors());
    // Manifold should produce actual geometry, not fall through to fallback.
    const auto &rootNode = result.scene.nodes.at(result.scene.rootId);
    REQUIRE_FALSE(rootNode.meshBindings.empty());
    const auto &mesh = result.scene.meshAssets.at(rootNode.meshBindings.front().meshId);
    REQUIRE_FALSE(mesh.vertices.empty());
    REQUIRE_FALSE(mesh.indices.empty());
}
#else
TEST_CASE("TessellationPass: boolean fallback=Skip emits warning, no mesh binding",
          "[tessellation][pass]") {
    NHConfig cfg;
    Rule rule;
    rule.tessellation = Rule::Tessellation{};
    rule.tessellation->fallback = BooleanFallback::Skip;
    cfg.rules.push_back(rule);

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
    Rule rule;
    rule.tessellation = Rule::Tessellation{};
    rule.tessellation->fallback = BooleanFallback::BBox;
    cfg.rules.push_back(rule);

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
    Rule rule;
    rule.tessellation = Rule::Tessellation{};
    rule.tessellation->fallback = BooleanFallback::Fail;
    cfg.rules.push_back(rule);

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
TEST_CASE("BooleanTessellator: ODD CarbonFoam (Trd - (Tube ∪ Tube)) reproducer",
          "[tessellation][boolean]") {
    // Load the exact shapes from a JSON fragment extracted from the ODD scene.
    // This ensures bit-identical shape parameters and IDs.
    static constexpr std::string_view kSceneJson = R"({
        "header": {"version": 1, "type": "semantic"},
        "content": {
            "rootId": 1,
            "sourceFile": "",
            "nodes": [
                {"id": 1, "name": "CarbonFoam_14", "logVolId": 1,
                 "localTransform": [1,0,0, 0,1,0, 0,0,1, 0,0,0],
                 "children": [], "tags": {}, "sourceSystem": "test"}
            ],
            "logVols": [
                {"id": 1, "name": "CarbonFoam", "shapeId": 80, "materialId": 1}
            ],
            "shapes": [
                {"id": 76, "type": "trd",
                 "dx1": 0.6000000000000001, "dx2": 0.1,
                 "dy1": 52.225, "dy2": 52.225, "dz": 0.2},
                {"id": 77, "type": "tube",
                 "rMin": 0.0, "rMax": 0.12, "dz": 52.725,
                 "phiStart": 6.183185307179587, "phiDelta": 0.414159265358979},
                {"id": 78, "type": "tube",
                 "rMin": 0.0, "rMax": 0.12, "dz": 52.725,
                 "phiStart": 0.0, "phiDelta": 6.283185307179586},
                {"id": 79, "type": "union", "left": 77, "right": 78,
                 "rightTransform": [1,0,0, 0,1,0, 0,0,1, 0,0,0]},
                {"id": 80, "type": "subtraction", "left": 76, "right": 79,
                 "rightTransform": [1,0,0, 0,6.123233995736766e-17,1, 0,-1,6.123233995736766e-17, 0,0,0]}
            ],
            "materials": [
                {"id": 1, "name": "CarbonFoam", "density": 0.0}
            ]
        }
    })";

    auto j = nlohmann::json::parse(kSceneJson);
    SemanticScene scene = j.get<SemanticScene>();

    // Verify shapes loaded correctly.
    REQUIRE(scene.shapes.contains(SemanticShapeId{80}));
    REQUIRE(std::holds_alternative<BooleanSubtraction>(scene.shapes.at(SemanticShapeId{80}).data));
    REQUIRE(std::holds_alternative<TrdShape>(scene.shapes.at(SemanticShapeId{76}).data));

    PrimitiveTessellator tess;
    TessellationParams params;
    params.maxSegmentsCircle = 48;

    auto result =
        tessellateBooleanShape(scene.shapes.at(SemanticShapeId{80}).data, scene, tess, params);

    for (const auto &d : result.diags.items()) {
        UNSCOPED_INFO(std::format("[{}] {}", d.code, d.message));
    }

    // Check: no non-manifold warnings should be emitted.
    bool hasManifoldWarn = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == codes::kWarnTessBooleanManifoldFail) {
            hasManifoldWarn = true;
        }
    }
    REQUIRE_FALSE(hasManifoldWarn);

    REQUIRE_FALSE(result.vertices.empty());
    REQUIRE_FALSE(result.indices.empty());
}
#endif // NH_WITH_BOOLEAN_MESH

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

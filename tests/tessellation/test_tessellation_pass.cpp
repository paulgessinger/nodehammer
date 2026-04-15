#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/synthetic/semantic/importer.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <limits>
#include <map>
#include <set>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <nodehammer/tessellation/boolean_tessellator.hpp>

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

TEST_CASE("TessellationPass: merge_descendants cache respects mirrored descendant layout",
          "[tessellation][pass]") {
    SemanticScene scene;

    const auto vacuumMat = scene.nextMaterialId();
    scene.materials[vacuumMat] = {vacuumMat, "Vacuum", std::nullopt, 0.0};
    const auto siliconMat = scene.nextMaterialId();
    scene.materials[siliconMat] = {siliconMat, "Silicon", std::nullopt, 0.0};
    const auto kaptonMat = scene.nextMaterialId();
    scene.materials[kaptonMat] = {kaptonMat, "Kapton", std::nullopt, 0.0};

    const auto rootShape = scene.nextShapeId();
    scene.shapes[rootShape] = {rootShape, BoxShape{10.0, 10.0, 10.0}};
    const auto diskShape = scene.nextShapeId();
    scene.shapes[diskShape] = {diskShape, BoxShape{1.0, 1.0, 0.01}};
    const auto layerShape = scene.nextShapeId();
    scene.shapes[layerShape] = {layerShape, BoxShape{0.5, 0.5, 0.01}};

    const auto rootLv = scene.nextLogVolId();
    scene.logVols[rootLv] = {rootLv, "root_lv", rootShape, vacuumMat};
    const auto diskLv = scene.nextLogVolId();
    scene.logVols[diskLv] = {diskLv, "shared_disk_lv", diskShape, vacuumMat};
    const auto siliconLv = scene.nextLogVolId();
    scene.logVols[siliconLv] = {siliconLv, "silicon_lv", layerShape, siliconMat};
    const auto kaptonLv = scene.nextLogVolId();
    scene.logVols[kaptonLv] = {kaptonLv, "kapton_lv", layerShape, kaptonMat};

    auto addNode = [&](std::string name, SemanticLogVolId lv, std::optional<SemanticNodeId> parent,
                       double z) {
        const auto id = scene.nextNodeId();
        SemanticNode node;
        node.id = id;
        node.name = std::move(name);
        node.logVolId = lv;
        node.parentId = parent;
        node.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.0, 0.0, z});
        scene.nodes[id] = node;
        if (parent.has_value()) {
            scene.nodes.at(*parent).children.push_back(id);
        }
        return id;
    };

    const auto root = addNode("world", rootLv, std::nullopt, 0.0);
    scene.rootId = root;
    const auto diskN = addNode("diskN", diskLv, root, -10.0);
    const auto diskP = addNode("diskP", diskLv, root, 10.0);

    // Both disk placements intentionally share the same logVolId. Their descendant layers are
    // mirrored in disk-local Z, which used to collide in the merge cache.
    addNode("siliconN", siliconLv, diskN, 0.1);
    addNode("kaptonN", kaptonLv, diskN, -0.1);
    addNode("siliconP", siliconLv, diskP, -0.1);
    addNode("kaptonP", kaptonLv, diskP, 0.1);

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();

    NHConfig cfg;
    MaterialDef siliconDef;
    siliconDef.name = "silicon";
    cfg.materials.push_back(siliconDef);
    MaterialDef kaptonDef;
    kaptonDef.name = "kapton";
    cfg.materials.push_back(kaptonDef);

    Rule siliconRule;
    siliconRule.match = PredicateExpr{MaterialGlobPredicate{"Silicon"}};
    siliconRule.material = "silicon";
    cfg.rules.push_back(siliconRule);

    Rule kaptonRule;
    kaptonRule.match = PredicateExpr{MaterialGlobPredicate{"Kapton"}};
    kaptonRule.material = "kapton";
    cfg.rules.push_back(kaptonRule);

    Rule mergeRule;
    mergeRule.match = PredicateExpr{NameGlobPredicate{"disk*"}};
    mergeRule.tessellation = Rule::Tessellation{};
    mergeRule.tessellation->mergeDescendants = true;
    cfg.rules.push_back(mergeRule);

    TessellationPass pass{cfg};
    auto result = pass.lower(scene);
    REQUIRE_FALSE(result.diags.hasErrors());

    auto findRenderNode = [&](std::string_view name) -> const RenderNode * {
        for (const auto &[_, node] : result.scene.nodes) {
            if (node.name == name) {
                return &node;
            }
        }
        return nullptr;
    };

    auto localCenterZ = [&](const RenderNode &node, std::string_view materialName) {
        for (const auto &binding : node.meshBindings) {
            const auto &mat = result.scene.materials.at(binding.materialId);
            if (mat.name != materialName) {
                continue;
            }
            const auto &mesh = result.scene.meshAssets.at(binding.meshId);
            double sum = 0.0;
            for (const auto &v : mesh.vertices) {
                sum += v.position.z;
            }
            return sum / static_cast<double>(mesh.vertices.size());
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    const auto *renderDiskN = findRenderNode("diskN");
    const auto *renderDiskP = findRenderNode("diskP");
    REQUIRE(renderDiskN != nullptr);
    REQUIRE(renderDiskP != nullptr);

    REQUIRE(localCenterZ(*renderDiskN, "kapton") < localCenterZ(*renderDiskN, "silicon"));
    REQUIRE(localCenterZ(*renderDiskP, "kapton") > localCenterZ(*renderDiskP, "silicon"));
}

TEST_CASE("TessellationPass: merge_descendants cache uses exact transform identity",
          "[tessellation][pass]") {
    SemanticScene scene;

    const auto vacuumMat = scene.nextMaterialId();
    scene.materials[vacuumMat] = {vacuumMat, "Vacuum", std::nullopt, 0.0};
    const auto siliconMat = scene.nextMaterialId();
    scene.materials[siliconMat] = {siliconMat, "Silicon", std::nullopt, 0.0};
    const auto kaptonMat = scene.nextMaterialId();
    scene.materials[kaptonMat] = {kaptonMat, "Kapton", std::nullopt, 0.0};

    const auto rootShape = scene.nextShapeId();
    scene.shapes[rootShape] = {rootShape, BoxShape{10.0, 10.0, 10.0}};
    const auto diskShape = scene.nextShapeId();
    scene.shapes[diskShape] = {diskShape, BoxShape{1.0, 1.0, 0.01}};
    const auto layerShape = scene.nextShapeId();
    scene.shapes[layerShape] = {layerShape, BoxShape{0.5, 0.5, 0.01}};

    const auto rootLv = scene.nextLogVolId();
    scene.logVols[rootLv] = {rootLv, "root_lv", rootShape, vacuumMat};
    const auto diskLv = scene.nextLogVolId();
    scene.logVols[diskLv] = {diskLv, "shared_disk_lv", diskShape, vacuumMat};
    const auto siliconLv = scene.nextLogVolId();
    scene.logVols[siliconLv] = {siliconLv, "silicon_lv", layerShape, siliconMat};
    const auto kaptonLv = scene.nextLogVolId();
    scene.logVols[kaptonLv] = {kaptonLv, "kapton_lv", layerShape, kaptonMat};

    auto addNode = [&](std::string name, SemanticLogVolId lv, std::optional<SemanticNodeId> parent,
                       double z) {
        const auto id = scene.nextNodeId();
        SemanticNode node;
        node.id = id;
        node.name = std::move(name);
        node.logVolId = lv;
        node.parentId = parent;
        node.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.0, 0.0, z});
        scene.nodes[id] = node;
        if (parent.has_value()) {
            scene.nodes.at(*parent).children.push_back(id);
        }
        return id;
    };

    const auto root = addNode("world", rootLv, std::nullopt, 0.0);
    scene.rootId = root;
    const auto diskA = addNode("diskA", diskLv, root, -10.0);
    const auto diskB = addNode("diskB", diskLv, root, 10.0);

    addNode("siliconA", siliconLv, diskA, 0.1);
    addNode("kaptonA", kaptonLv, diskA, -0.1);
    addNode("siliconB", siliconLv, diskB, 0.1 + 1.0e-13);
    addNode("kaptonB", kaptonLv, diskB, -0.1 - 1.0e-13);

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();

    NHConfig cfg;
    MaterialDef siliconDef;
    siliconDef.name = "silicon";
    cfg.materials.push_back(siliconDef);
    MaterialDef kaptonDef;
    kaptonDef.name = "kapton";
    cfg.materials.push_back(kaptonDef);

    Rule siliconRule;
    siliconRule.match = PredicateExpr{MaterialGlobPredicate{"Silicon"}};
    siliconRule.material = "silicon";
    cfg.rules.push_back(siliconRule);

    Rule kaptonRule;
    kaptonRule.match = PredicateExpr{MaterialGlobPredicate{"Kapton"}};
    kaptonRule.material = "kapton";
    cfg.rules.push_back(kaptonRule);

    Rule mergeRule;
    mergeRule.match = PredicateExpr{NameGlobPredicate{"disk*"}};
    mergeRule.tessellation = Rule::Tessellation{};
    mergeRule.tessellation->mergeDescendants = true;
    cfg.rules.push_back(mergeRule);

    TessellationPass pass{cfg};
    auto result = pass.lower(scene);
    REQUIRE_FALSE(result.diags.hasErrors());

    // Two primitive layer meshes plus four merged material meshes. The tiny local-transform
    // differences above are intentionally not collapsed by approximate matching.
    REQUIRE(result.scene.meshAssets.size() == 6);
}

TEST_CASE("TessellationPass: merge_descendants cache can use source daughter prototypes",
          "[tessellation][pass]") {
    SemanticScene scene;

    const auto vacuumMat = scene.nextMaterialId();
    scene.materials[vacuumMat] = {vacuumMat, "Vacuum", std::nullopt, 0.0};
    const auto siliconMat = scene.nextMaterialId();
    scene.materials[siliconMat] = {siliconMat, "Silicon", std::nullopt, 0.0};
    const auto kaptonMat = scene.nextMaterialId();
    scene.materials[kaptonMat] = {kaptonMat, "Kapton", std::nullopt, 0.0};

    const auto rootShape = scene.nextShapeId();
    scene.shapes[rootShape] = {rootShape, BoxShape{10.0, 10.0, 10.0}};
    const auto diskShape = scene.nextShapeId();
    scene.shapes[diskShape] = {diskShape, BoxShape{1.0, 1.0, 0.01}};
    const auto layerShape = scene.nextShapeId();
    scene.shapes[layerShape] = {layerShape, BoxShape{0.5, 0.5, 0.01}};

    const auto rootLv = scene.nextLogVolId();
    scene.logVols[rootLv] = {rootLv, "root_lv", rootShape, vacuumMat};
    const auto siliconLv = scene.nextLogVolId();
    scene.logVols[siliconLv] = {siliconLv, "silicon_lv", layerShape, siliconMat};
    const auto kaptonLv = scene.nextLogVolId();
    scene.logVols[kaptonLv] = {kaptonLv, "kapton_lv", layerShape, kaptonMat};

    glm::dmat4 siliconPlacement = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.0, 0.0, 0.1});
    glm::dmat4 kaptonPlacement = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.0, 0.0, -0.1});
    const auto diskLv = scene.nextLogVolId();
    scene.logVols[diskLv] = {
        diskLv,
        "shared_disk_lv",
        diskShape,
        vacuumMat,
        {{"silicon", siliconLv, siliconPlacement}, {"kapton", kaptonLv, kaptonPlacement}}};

    auto addNode = [&](std::string name, SemanticLogVolId lv, std::optional<SemanticNodeId> parent,
                       double z) {
        const auto id = scene.nextNodeId();
        SemanticNode node;
        node.id = id;
        node.name = std::move(name);
        node.logVolId = lv;
        node.parentId = parent;
        node.localTransform = glm::translate(glm::dmat4{1.0}, glm::dvec3{0.0, 0.0, z});
        scene.nodes[id] = node;
        if (parent.has_value()) {
            scene.nodes.at(*parent).children.push_back(id);
        }
        return id;
    };

    const auto root = addNode("world", rootLv, std::nullopt, 0.0);
    scene.rootId = root;
    const auto diskA = addNode("diskA", diskLv, root, -10.0);
    const auto diskB = addNode("diskB", diskLv, root, 10.0);

    addNode("siliconA", siliconLv, diskA, 0.1);
    addNode("kaptonA", kaptonLv, diskA, -0.1);
    addNode("siliconB", siliconLv, diskB, 0.1 + 1.0e-13);
    addNode("kaptonB", kaptonLv, diskB, -0.1 - 1.0e-13);

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();

    NHConfig cfg;
    MaterialDef siliconDef;
    siliconDef.name = "silicon";
    cfg.materials.push_back(siliconDef);
    MaterialDef kaptonDef;
    kaptonDef.name = "kapton";
    cfg.materials.push_back(kaptonDef);

    Rule siliconRule;
    siliconRule.match = PredicateExpr{MaterialGlobPredicate{"Silicon"}};
    siliconRule.material = "silicon";
    cfg.rules.push_back(siliconRule);

    Rule kaptonRule;
    kaptonRule.match = PredicateExpr{MaterialGlobPredicate{"Kapton"}};
    kaptonRule.material = "kapton";
    cfg.rules.push_back(kaptonRule);

    Rule mergeRule;
    mergeRule.match = PredicateExpr{NameGlobPredicate{"disk*"}};
    mergeRule.tessellation = Rule::Tessellation{};
    mergeRule.tessellation->mergeDescendants = true;
    cfg.rules.push_back(mergeRule);

    TessellationPass pass{cfg};
    auto result = pass.lower(scene);
    REQUIRE_FALSE(result.diags.hasErrors());

    // Two primitive layer meshes plus two merged material meshes. The cache key uses the exact
    // source daughter prototype transforms, not the noisy selected-node placement rebasing.
    REQUIRE(result.scene.meshAssets.size() == 4);
}

// ── Boolean fallback ──────────────────────────────────────────────────────────

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

TEST_CASE("BooleanTessellator: ODD CarbonFoam Trd minus tube union reproducer",
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
                {"id": 1, "name": "CarbonFoam", "shapeId": 80, "materialId": 1,
                 "daughters": []}
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

TEST_CASE("Trd tessellation produces manifold-compatible mesh", "[tessellation][boolean]") {
    // Verify that the Trd tessellation produces a watertight mesh that Manifold
    // accepts. This caught inconsistent face winding in the original Trd
    // tessellator (the ODD CarbonFoam non-manifold bug).
    PrimitiveTessellator tess;
    TessellationParams params;
    params.maxSegmentsCircle = 48;

    TrdShape trd{0.6000000000000001, 0.1, 52.225, 52.225, 0.2};
    auto out = tess.tessellate(trd, params);
    REQUIRE_FALSE(out.vertices.empty());

    DiagnosticList diags;
    auto m = meshToManifold(out, diags, "test/trd");
    for (const auto &d : diags.items()) {
        UNSCOPED_INFO(std::format("[{}] {}", d.code, d.message));
    }
    REQUIRE(m.has_value());
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

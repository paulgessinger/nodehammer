#include <ir/diagnostic_codes.hpp>
#include <ir/provenance.hpp>
#include <ir/synthetic/semantic/importer.hpp>

#include <glm/glm.hpp>

namespace nodehammer::ir {

namespace {

/// Add a shape+material+logvol to the scene and return the logvol ID.
SemanticLogVolId addVolume(SemanticScene &scene, std::string_view lvName,
                           SemanticShapeVariant shapeData, std::string_view matName,
                           glm::vec3 color = {0.75f, 0.75f, 0.85f}, double density = 0.0) {
    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = SemanticShape{shapeId, std::move(shapeData)};

    auto matId = scene.nextMaterialId();
    SourceMaterial mat;
    mat.id = matId;
    mat.name = std::string{matName};
    mat.color = color;
    mat.density = density;
    scene.materials[matId] = mat;

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = SemanticLogicalVolume{lvId, std::string{lvName}, shapeId, matId};

    return lvId;
}

/// Add a node to the scene, wire up the parent link, and return its ID.
SemanticNodeId addNode(SemanticScene &scene, std::string_view name, SemanticLogVolId lvId,
                       std::optional<SemanticNodeId> parentId = std::nullopt,
                       glm::dmat4 localTransform = glm::dmat4{1.0}) {
    auto nodeId = scene.nextNodeId();
    SemanticNode node;
    node.id = nodeId;
    node.name = std::string{name};
    node.logVolId = lvId;
    node.localTransform = localTransform;
    node.parentId = parentId;
    node.sourceSystem = "synthetic";
    scene.nodes[nodeId] = node;

    if (parentId.has_value()) {
        scene.nodes.at(*parentId).children.push_back(nodeId);
    }

    return nodeId;
}

} // namespace

// ── SyntheticSceneBuilder ─────────────────────────────────────────────────────

SemanticScene SyntheticSceneBuilder::buildSingleBox() {
    SemanticScene scene;
    auto lvId = addVolume(scene, "boxLV", BoxShape{10.0, 10.0, 10.0}, "aluminum",
                          {0.75f, 0.75f, 0.85f}, 2.7);
    auto nodeId = addNode(scene, "world", lvId);
    scene.rootId = nodeId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

SemanticScene SyntheticSceneBuilder::buildNestedBoxes() {
    SemanticScene scene;

    auto outerLv =
        addVolume(scene, "worldLV", BoxShape{50.0, 50.0, 50.0}, "air", {0.9f, 0.9f, 0.9f}, 0.0012);
    auto innerLv = addVolume(scene, "innerLV", BoxShape{10.0, 10.0, 10.0}, "aluminum",
                             {0.75f, 0.75f, 0.85f}, 2.7);

    auto rootId = addNode(scene, "world", outerLv);
    scene.rootId = rootId;

    glm::dmat4 childTransform{1.0};
    childTransform[3] = glm::dvec4{0.0, 0.0, 100.0, 1.0}; // translate 100 mm along Z
    addNode(scene, "inner", innerLv, rootId, childTransform);

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

SemanticScene SyntheticSceneBuilder::buildTubeInBox() {
    SemanticScene scene;

    auto outerLv =
        addVolume(scene, "worldLV", BoxShape{50.0, 50.0, 50.0}, "air", {0.9f, 0.9f, 0.9f}, 0.0012);
    auto tubeLv = addVolume(scene, "tubeLV", TubeShape{0.0, 5.0, 10.0}, "aluminum",
                            {0.75f, 0.75f, 0.85f}, 2.7);

    auto rootId = addNode(scene, "world", outerLv);
    scene.rootId = rootId;
    addNode(scene, "tube", tubeLv, rootId);

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

SemanticScene SyntheticSceneBuilder::buildBooleanSubtraction() {
    SemanticScene scene;

    // Register the two operand shapes (not bound to logical volumes).
    auto outerShapeId = scene.nextShapeId();
    scene.shapes[outerShapeId] = SemanticShape{outerShapeId, BoxShape{20.0, 20.0, 20.0}};

    auto innerShapeId = scene.nextShapeId();
    scene.shapes[innerShapeId] = SemanticShape{innerShapeId, BoxShape{10.0, 10.0, 10.0}};

    // The composite boolean shape.
    auto boolShapeId = scene.nextShapeId();
    scene.shapes[boolShapeId] =
        SemanticShape{boolShapeId, BooleanSubtraction{outerShapeId, innerShapeId}};

    auto matId = scene.nextMaterialId();
    SourceMaterial mat;
    mat.id = matId;
    mat.name = "aluminum";
    mat.color = glm::vec3{0.75f, 0.75f, 0.85f};
    mat.density = 2.7;
    scene.materials[matId] = mat;

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = SemanticLogicalVolume{lvId, "boolLV", boolShapeId, matId};

    auto nodeId = addNode(scene, "world", lvId);
    scene.rootId = nodeId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

ImportResult SyntheticSceneBuilder::buildWithDiagnostics() {
    SemanticScene scene;
    DiagnosticList diags;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = SemanticShape{shapeId, UnknownShape{"SyntheticUnknown"}};

    auto matId = scene.nextMaterialId();
    SourceMaterial mat;
    mat.id = matId;
    mat.name = "aluminum";
    mat.color = glm::vec3{0.75f, 0.75f, 0.85f};
    mat.density = 2.7;
    scene.materials[matId] = mat;

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = SemanticLogicalVolume{lvId, "unknownLV", shapeId, matId};

    auto nodeId = scene.nextNodeId();
    SemanticNode node;
    node.id = nodeId;
    node.name = "world";
    node.logVolId = lvId;
    node.sourceSystem = "synthetic";
    node.degradation.set(DegradationBit::UnknownShape);
    scene.nodes[nodeId] = node;
    scene.rootId = nodeId;
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();

    diags.warn(codes::kWarnImportUnknownShape, "Unknown shape type: SyntheticUnknown", "world");

    return ImportResult{std::move(scene), std::move(diags)};
}

// ── SyntheticImporter ─────────────────────────────────────────────────────────

std::string_view SyntheticImporter::formatName() const noexcept { return "synthetic"; }

std::vector<std::string> SyntheticImporter::supportedExtensions() const { return {}; }

ImportResult SyntheticImporter::import([[maybe_unused]] const std::filesystem::path &path) const {
    return ImportResult{SyntheticSceneBuilder::buildSingleBox(), {}};
}

} // namespace nodehammer::ir

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <detail/zstd_io.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/semantic.hpp>
#include <ir/semantic/importer.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <numbers>
#include <span>

using namespace nodehammer;
using namespace nodehammer::ir;
using Catch::Approx;

namespace {

/// Build a minimal scene with one root node, one logVol, one box shape, one material.
ir::semantic::Scene makeMinimalScene() {
    ir::semantic::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, ir::semantic::BoxShape{5.0, 10.0, 15.0}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "iron", nodehammer::ir::Color3{0.5f, 0.5f, 0.5f}, 7.87};

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "ironBox", shapeId, matId};

    auto nodeId = scene.nextNodeId();
    ir::semantic::Node node;
    node.id = nodeId;
    node.name = "root";
    node.logVolId = lvId;
    node.sourceSystem = "test";
    scene.nodes[nodeId] = node;
    scene.rootId = nodeId;
    scene.sourceFile = "/test/input.gdml";

    return scene;
}

} // namespace

TEST_CASE("FlatBuffer roundtrip: minimal scene", "[ir][flatbuffer]") {
    auto original = makeMinimalScene();

    auto bytes = semanticSceneToBytes(original);
    REQUIRE(!bytes.empty());

    auto restored = semanticSceneFromBytes(std::as_bytes(std::span{bytes}));

    REQUIRE(restored.sourceFile == original.sourceFile);
    REQUIRE(restored.nodes.size() == original.nodes.size());
    REQUIRE(restored.shapes.size() == original.shapes.size());
    REQUIRE(restored.logVols.size() == original.logVols.size());
    REQUIRE(restored.materials.size() == original.materials.size());

    // Check root node semantics (node ID may be remapped by serialization order).
    REQUIRE(restored.nodes.contains(restored.rootId));
    const auto &resNode = restored.nodes.at(restored.rootId);
    REQUIRE(resNode.name == "root");
    REQUIRE(resNode.logVolId.value != 0);
    REQUIRE(resNode.sourceSystem == "test");

    // Check shape (box)
    const auto &origShape = original.shapes.begin()->second;
    const auto &resShape = restored.shapes.at(origShape.id);
    const auto *origBox = std::get_if<ir::semantic::BoxShape>(&origShape.data);
    const auto *resBox = std::get_if<ir::semantic::BoxShape>(&resShape.data);
    REQUIRE(origBox != nullptr);
    REQUIRE(resBox != nullptr);
    REQUIRE(resBox->dx == Approx(origBox->dx));
    REQUIRE(resBox->dy == Approx(origBox->dy));
    REQUIRE(resBox->dz == Approx(origBox->dz));

    // Check material with color
    const auto &origMat = original.materials.begin()->second;
    const auto &resMat = restored.materials.at(origMat.id);
    REQUIRE(resMat.name == origMat.name);
    REQUIRE(resMat.density == Approx(origMat.density));
    REQUIRE(resMat.color.has_value());
    REQUIRE(resMat.color->r == Approx(origMat.color->r));
}

TEST_CASE("FlatBuffer roundtrip: all shape types", "[ir][flatbuffer]") {
    ir::semantic::Scene scene;
    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "air", std::nullopt, 0.001};

    auto addShape = [&](ir::semantic::ShapeVariant data) {
        auto id = scene.nextShapeId();
        scene.shapes[id] = {id, std::move(data)};
        auto lvId = scene.nextLogVolId();
        scene.logVols[lvId] = {lvId, "lv", id, matId};
        return id;
    };

    addShape(ir::semantic::BoxShape{1.0, 2.0, 3.0});
    addShape(ir::semantic::TubeShape{5.0, 10.0, 20.0, 0.1, 3.0});
    addShape(ir::semantic::ConeShape{1.0, 5.0, 2.0, 8.0, 15.0, 0.0, 2.0 * std::numbers::pi});
    addShape(ir::semantic::TrdShape{1.0, 2.0, 3.0, 4.0, 5.0});
    addShape(ir::semantic::ParaShape{1.0, 2.0, 3.0, 0.1, 0.2, 0.3});

    ir::semantic::PconShape pcon;
    pcon.phiStart = 0.5;
    pcon.phiDelta = 3.0;
    pcon.sections = {{-10.0, 1.0, 5.0}, {0.0, 2.0, 6.0}, {10.0, 1.0, 5.0}};
    addShape(pcon);

    ir::semantic::PgonShape pgon;
    pgon.phiStart = 0.0;
    pgon.phiDelta = 2.0 * std::numbers::pi;
    pgon.nSides = 6;
    pgon.sections = {{-5.0, 1.0, 3.0}, {5.0, 1.0, 3.0}};
    addShape(pgon);

    addShape(ir::semantic::TorusShape{1.0, 3.0, 10.0, 0.0, 2.0 * std::numbers::pi});

    ir::semantic::TessellatedShape tess;
    ir::semantic::TessellatedShape::Triangle tri;
    tri.vertices[0] = {0.0, 0.0, 0.0};
    tri.vertices[1] = {1.0, 0.0, 0.0};
    tri.vertices[2] = {0.0, 1.0, 0.0};
    tess.triangles.push_back(tri);
    addShape(tess);

    // Boolean shapes: need two operand shapes first
    auto leftId = addShape(ir::semantic::BoxShape{1.0, 1.0, 1.0});
    auto rightId = addShape(ir::semantic::BoxShape{0.5, 0.5, 0.5});
    nodehammer::ir::Mat4 boolTransform;
    boolTransform[3] = nodehammer::ir::Vec4{1.0, 2.0, 3.0, 1.0};

    addShape(ir::semantic::BooleanUnion{leftId, rightId, boolTransform});
    addShape(ir::semantic::BooleanIntersection{leftId, rightId, nodehammer::ir::Mat4{}});
    addShape(ir::semantic::BooleanSubtraction{leftId, rightId, boolTransform});

    addShape(ir::semantic::UnknownShape{"TGeoArb8"});

    // Minimal node to make it a valid scene
    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "rootLv", scene.shapes.begin()->first, matId};
    auto nodeId = scene.nextNodeId();
    ir::semantic::Node node;
    node.id = nodeId;
    node.name = "root";
    node.logVolId = lvId;
    scene.nodes[nodeId] = node;
    scene.rootId = nodeId;

    auto bytes = semanticSceneToBytes(scene);
    auto restored = semanticSceneFromBytes(std::as_bytes(std::span{bytes}));

    REQUIRE(restored.shapes.size() == scene.shapes.size());

    // Verify specific shape types survived the roundtrip
    for (const auto &[id, origShape] : scene.shapes) {
        REQUIRE(restored.shapes.contains(id));
        const auto &resShape = restored.shapes.at(id);
        REQUIRE(origShape.data.index() == resShape.data.index());

        // Spot-check a few shapes
        if (const auto *origTube = std::get_if<ir::semantic::TubeShape>(&origShape.data)) {
            const auto *resTube = std::get_if<ir::semantic::TubeShape>(&resShape.data);
            REQUIRE(resTube != nullptr);
            REQUIRE(resTube->rMin == Approx(origTube->rMin));
            REQUIRE(resTube->rMax == Approx(origTube->rMax));
            REQUIRE(resTube->dz == Approx(origTube->dz));
            REQUIRE(resTube->phiStart == Approx(origTube->phiStart));
            REQUIRE(resTube->phiDelta == Approx(origTube->phiDelta));
        }
        if (const auto *origPcon = std::get_if<ir::semantic::PconShape>(&origShape.data)) {
            const auto *resPcon = std::get_if<ir::semantic::PconShape>(&resShape.data);
            REQUIRE(resPcon != nullptr);
            REQUIRE(resPcon->sections.size() == origPcon->sections.size());
            for (std::size_t i = 0; i < origPcon->sections.size(); ++i) {
                REQUIRE(resPcon->sections[i].z == Approx(origPcon->sections[i].z));
                REQUIRE(resPcon->sections[i].rMin == Approx(origPcon->sections[i].rMin));
                REQUIRE(resPcon->sections[i].rMax == Approx(origPcon->sections[i].rMax));
            }
        }
        if (const auto *origBoolU = std::get_if<ir::semantic::BooleanUnion>(&origShape.data)) {
            const auto *resBoolU = std::get_if<ir::semantic::BooleanUnion>(&resShape.data);
            REQUIRE(resBoolU != nullptr);
            REQUIRE(resBoolU->left == origBoolU->left);
            REQUIRE(resBoolU->right == origBoolU->right);
            // Check non-identity transform was preserved
            REQUIRE(resBoolU->rightTransform[3][0] == Approx(1.0));
            REQUIRE(resBoolU->rightTransform[3][1] == Approx(2.0));
            REQUIRE(resBoolU->rightTransform[3][2] == Approx(3.0));
        }
        if (std::get_if<ir::semantic::BooleanIntersection>(&origShape.data) != nullptr) {
            const auto *resBoolI = std::get_if<ir::semantic::BooleanIntersection>(&resShape.data);
            REQUIRE(resBoolI != nullptr);
            // Identity transform: check diagonal
            REQUIRE(resBoolI->rightTransform[0][0] == Approx(1.0));
            REQUIRE(resBoolI->rightTransform[3][0] == Approx(0.0));
        }
        if (const auto *origUnk = std::get_if<ir::semantic::UnknownShape>(&origShape.data)) {
            const auto *resUnk = std::get_if<ir::semantic::UnknownShape>(&resShape.data);
            REQUIRE(resUnk != nullptr);
            REQUIRE(resUnk->originalType == origUnk->originalType);
        }
    }
}

TEST_CASE("FlatBuffer roundtrip: complex scene with hierarchy", "[ir][flatbuffer]") {
    ir::semantic::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, ir::semantic::BoxShape{1.0, 1.0, 1.0}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "vacuum", std::nullopt, 0.0};

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "worldLV", shapeId, matId};

    // Root
    auto rootId = scene.nextNodeId();
    ir::semantic::Node root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = lvId;
    root.tags["detector"] = "tracker";
    root.tags["sensitive"] = "true";
    root.sourceSystem = "dd4hep";
    scene.nodes[rootId] = root;
    scene.rootId = rootId;

    // Child with non-identity transform
    nodehammer::ir::Mat4 childTransform;
    childTransform[3] = nodehammer::ir::Vec4{100.0, 200.0, 300.0, 1.0};
    // Add a rotation component too
    double angle = 0.5; // radians
    childTransform[0][0] = std::cos(angle);
    childTransform[0][1] = std::sin(angle);
    childTransform[1][0] = -std::sin(angle);
    childTransform[1][1] = std::cos(angle);

    auto childId = scene.nextNodeId();
    ir::semantic::Node child;
    child.id = childId;
    child.name = "sensor_0";
    child.logVolId = lvId;
    child.localTransform = childTransform;
    child.parentId = rootId;
    child.degradation.bits =
        decltype(child.degradation.bits)(0b0101); // ir::semantic::UnknownShape + TransformApprox
    scene.nodes[childId] = child;
    scene.nodes[rootId].children.push_back(childId);

    // Grandchild with identity transform
    auto grandchildId = scene.nextNodeId();
    ir::semantic::Node grandchild;
    grandchild.id = grandchildId;
    grandchild.name = "pixel_0";
    grandchild.logVolId = lvId;
    grandchild.parentId = childId;
    scene.nodes[grandchildId] = grandchild;
    scene.nodes[childId].children.push_back(grandchildId);

    scene.sourceFile = "/test/detector.xml";

    auto bytes = semanticSceneToBytes(scene);
    auto restored = semanticSceneFromBytes(std::as_bytes(std::span{bytes}));

    REQUIRE(restored.nodes.size() == 3);
    REQUIRE(restored.sourceFile == "/test/detector.xml");

    auto findNodeIdByName = [&](std::string_view name) -> std::optional<ir::semantic::NodeId> {
        for (const auto &[id, node] : restored.nodes) {
            if (node.name == name) {
                return id;
            }
        }
        return std::nullopt;
    };

    const auto resRootId = findNodeIdByName("world");
    const auto resChildId = findNodeIdByName("sensor_0");
    const auto resGrandchildId = findNodeIdByName("pixel_0");
    REQUIRE(resRootId.has_value());
    REQUIRE(resChildId.has_value());
    REQUIRE(resGrandchildId.has_value());
    REQUIRE(restored.rootId == *resRootId);

    // Root tags
    const auto &resRoot = restored.nodes.at(*resRootId);
    REQUIRE(resRoot.tags.size() == 2);
    REQUIRE(resRoot.tags.at("detector") == "tracker");
    REQUIRE(resRoot.tags.at("sensitive") == "true");
    REQUIRE(resRoot.sourceSystem == "dd4hep");
    REQUIRE(resRoot.children.size() == 1);
    REQUIRE(resRoot.children[0] == *resChildId);

    // Child transform
    const auto &resChild = restored.nodes.at(*resChildId);
    REQUIRE(resChild.parentId.has_value());
    REQUIRE(*resChild.parentId == *resRootId);
    REQUIRE(resChild.localTransform[3][0] == Approx(100.0));
    REQUIRE(resChild.localTransform[3][1] == Approx(200.0));
    REQUIRE(resChild.localTransform[3][2] == Approx(300.0));
    REQUIRE(resChild.localTransform[0][0] == Approx(std::cos(0.5)));
    REQUIRE(resChild.localTransform[0][1] == Approx(std::sin(0.5)));

    // Degradation flags
    REQUIRE(resChild.degradation.bits.to_ulong() == 0b0101);

    // Grandchild: identity transform, has parent
    const auto &resGrandchild = restored.nodes.at(*resGrandchildId);
    REQUIRE(resGrandchild.parentId.has_value());
    REQUIRE(*resGrandchild.parentId == *resChildId);
    // Identity transform check
    REQUIRE(resGrandchild.localTransform[0][0] == Approx(1.0));
    REQUIRE(resGrandchild.localTransform[3][0] == Approx(0.0));

    // Material without color
    const auto &resMat = restored.materials.at(matId);
    REQUIRE(resMat.name == "vacuum");
    REQUIRE(!resMat.color.has_value());
}

TEST_CASE("FlatBuffer: file identifier check", "[ir][flatbuffer]") {
    auto scene = makeMinimalScene();
    auto bytes = semanticSceneToBytes(scene);

    // Valid buffer should have "NHSM" identifier
    REQUIRE(bytes.size() >= 8);
    REQUIRE(flatbuffers::BufferHasIdentifier(bytes.data(), fbs::SemanticSceneIdentifier()));

    // Garbage input should throw
    std::vector<uint8_t> garbage = {0, 1, 2, 3, 4, 5, 6, 7};
    REQUIRE_THROWS_AS(semanticSceneFromBytes(std::as_bytes(std::span{garbage})),
                      std::runtime_error);
}

TEST_CASE("FlatBuffer roundtrip: logical volume with daughters", "[ir][flatbuffer]") {
    ir::semantic::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId,
                             ir::semantic::TubeShape{0.0, 5.0, 10.0, 0.0, 2.0 * std::numbers::pi}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "silicon", nodehammer::ir::Color3{0.3f, 0.3f, 0.8f}, 2.33};

    auto childLvId = scene.nextLogVolId();
    scene.logVols[childLvId] = {childLvId, "sensorLV", shapeId, matId};

    nodehammer::ir::Mat4 placement;
    placement[3] = nodehammer::ir::Vec4{0.0, 0.0, 50.0, 1.0};

    auto parentLvId = scene.nextLogVolId();
    scene.logVols[parentLvId] = {
        parentLvId,
        "moduleLV",
        shapeId,
        matId,
        {{.name = "sensor_phys", .logVolId = childLvId, .localTransform = placement}}};

    auto nodeId = scene.nextNodeId();
    ir::semantic::Node node;
    node.id = nodeId;
    node.name = "root";
    node.logVolId = parentLvId;
    scene.nodes[nodeId] = node;
    scene.rootId = nodeId;

    auto bytes = semanticSceneToBytes(scene);
    auto restored = semanticSceneFromBytes(std::as_bytes(std::span{bytes}));

    const auto &resLv = restored.logVols.at(parentLvId);
    REQUIRE(resLv.daughters.size() == 1);
    REQUIRE(resLv.daughters[0].name == "sensor_phys");
    REQUIRE(resLv.daughters[0].logVolId == childLvId);
    REQUIRE(resLv.daughters[0].localTransform[3][2] == Approx(50.0));
}

TEST_CASE("FlatBuffer roundtrip: Layer 1 API composes", "[ir][flatbuffer]") {
    // Verify that the Layer 1 API (offset-based) works correctly
    auto scene = makeMinimalScene();

    flatbuffers::FlatBufferBuilder builder{1024};
    auto offset = semanticSceneToFlatBuffer(builder, scene);
    fbs::FinishSemanticSceneBuffer(builder, offset);

    auto *ptr = builder.GetBufferPointer();

    const auto *fb = fbs::GetSemanticScene(ptr);
    auto restored = semanticSceneFromFlatBuffer(*fb);

    REQUIRE(restored.sourceFile == scene.sourceFile);
    REQUIRE(restored.nodes.size() == scene.nodes.size());
    REQUIRE(restored.nodes.contains(restored.rootId));
    REQUIRE(restored.nodes.at(restored.rootId).name == "root");
}

TEST_CASE("FlatBuffer zstd bytes: nhb.zst roundtrip", "[ir][flatbuffer]") {
    auto scene = makeMinimalScene();
    auto bytes = semanticSceneToBytes(scene);

    const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = std::filesystem::temp_directory_path() /
                     std::filesystem::path{"nodehammer_flatbuffer_roundtrip_test_" +
                                           std::to_string(uniqueSuffix) + ".nhb.zst"};
    nodehammer::detail::zstd_io::writeBytesToFile(tmp, std::as_bytes(std::span{bytes}));

    auto decoded = nodehammer::detail::zstd_io::readBytesFromFile(tmp);
    auto restored = semanticSceneFromBytes(decoded);

    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    REQUIRE(restored.sourceFile == scene.sourceFile);
    REQUIRE(restored.nodes.contains(restored.rootId));
    REQUIRE(restored.nodes.at(restored.rootId).name == "root");
}

TEST_CASE("FlatBuffer importer resolves compound .nhb.zst extension", "[ir][flatbuffer]") {
    const auto reg = ImporterRegistry::makeDefault();
    const auto *imp = reg.resolve("anything.nhb.zst", "");

    REQUIRE(imp != nullptr);
    REQUIRE(imp->formatName() == "nhb");
}

TEST_CASE("FlatBufferImporter::importFromBytes accepts raw .nhb bytes", "[ir][flatbuffer][bytes]") {
    auto scene = makeMinimalScene();
    auto raw = semanticSceneToBytes(scene);

    auto result = FlatBufferImporter::importFromBytes("scene.nhb", std::as_bytes(std::span{raw}));
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(!result.diags.hasErrors());
    REQUIRE(result.scene.nodes.contains(result.scene.rootId));
    REQUIRE(result.scene.nodes.at(result.scene.rootId).name == "root");
}

TEST_CASE("FlatBufferImporter::importFromBytes decompresses .nhb.zst bytes",
          "[ir][flatbuffer][bytes]") {
    auto scene = makeMinimalScene();
    auto raw = semanticSceneToBytes(scene);
    auto compressed = nodehammer::detail::zstd_io::compress(std::as_bytes(std::span{raw}));

    auto result = FlatBufferImporter::importFromBytes("scene.nhb.zst",
                                                      std::span<const std::byte>{compressed});
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(!result.diags.hasErrors());
    REQUIRE(result.scene.nodes.contains(result.scene.rootId));
    REQUIRE(result.scene.nodes.at(result.scene.rootId).name == "root");
}

TEST_CASE("FlatBufferImporter::importFromBytes matches path-based import",
          "[ir][flatbuffer][bytes]") {
    auto scene = makeMinimalScene();
    auto raw = semanticSceneToBytes(scene);

    const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = std::filesystem::temp_directory_path() /
                     std::filesystem::path{"nodehammer_fb_bytes_path_eq_" +
                                           std::to_string(uniqueSuffix) + ".nhb"};
    nodehammer::detail::zstd_io::writeBytesToFile(tmp, std::as_bytes(std::span{raw}));

    FlatBufferImporter imp;
    auto via_path = imp.import(tmp);
    auto via_bytes =
        FlatBufferImporter::importFromBytes("scene.nhb", std::as_bytes(std::span{raw}));

    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    REQUIRE_FALSE(via_path.diags.hasErrors());
    REQUIRE_FALSE(via_bytes.diags.hasErrors());
    REQUIRE(via_path.scene.nodes.size() == via_bytes.scene.nodes.size());
    REQUIRE(via_path.scene.materials.size() == via_bytes.scene.materials.size());
    REQUIRE(via_path.scene.shapes.size() == via_bytes.scene.shapes.size());
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ir/fb/render/flatbuffer.hpp>
#include <ir/render.hpp>

#include <cstdint>
#include <span>
#include <vector>

using namespace nodehammer;
using namespace nodehammer::ir;
using Catch::Approx;

namespace {

MeshAsset makeMesh(MeshAssetId id, const char *name) {
    MeshAsset m;
    m.id = id;
    m.name = name;
    m.vertices = {
        Vertex{glm::vec3{0.f, 0.f, 0.f}, glm::vec3{0.f, 0.f, 1.f}},
        Vertex{glm::vec3{1.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f}},
        Vertex{glm::vec3{0.f, 1.f, 0.f}, glm::vec3{1.f, 0.f, 0.f}},
        Vertex{glm::vec3{1.f, 1.f, 2.f}, glm::vec3{0.577f, 0.577f, 0.577f}},
    };
    m.indices = {0, 1, 2, 1, 3, 2};
    m.provenance.sourceSystem = "tessellator";
    m.provenance.sourceName = name;
    m.provenance.sourceFile = "/scene.gdml";
    m.provenance.degradation.set(DegradationBit::UnknownShape);  // bit 0
    m.provenance.degradation.set(DegradationBit::TruncatedName); // bit 3
    return m;
}

/// A RenderScene with two meshes, a fully-populated material, a bare material,
/// and a 3-node hierarchy carrying transforms / bindings / semantic refs.
RenderScene makeScene() {
    RenderScene scene;

    const auto meshA = scene.nextMeshId();
    const auto meshB = scene.nextMeshId();
    scene.meshAssets[meshA] = makeMesh(meshA, "boxMesh");
    scene.meshAssets[meshB] = makeMesh(meshB, "tubeMesh");
    // meshB carries a stack-average prefilter hint; meshA leaves it nullopt so
    // the round-trip covers both the present and absent paths.
    scene.meshAssets[meshB].stackAverage = StackAverage{glm::vec3{0.12f, 0.34f, 0.56f}, 0.75f};

    // Material 1: every optional KHR field set.
    const auto matFull = scene.nextMaterialId();
    {
        RenderMaterial m;
        m.id = matFull;
        m.name = "glassy";
        m.baseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
        m.metallicFactor = 0.7f;
        m.roughnessFactor = 0.3f;
        m.emissiveFactor = {0.5f, 0.6f, 0.7f};
        m.alphaMode = "BLEND";
        m.alphaCutoff = 0.25f;
        m.doubleSided = true;
        m.ior = 1.5f;
        m.transmissionFactor = 0.9f;
        m.clearcoatFactor = 0.8f;
        m.clearcoatRoughnessFactor = 0.1f;
        m.anisotropyStrength = 0.2f;
        m.anisotropyRotation = 0.05f;
        m.specularFactor = 0.6f;
        m.specularColorFactor = glm::vec3{0.9f, 0.8f, 0.7f};
        scene.materials[matFull] = m;
    }
    // Material 2: all optionals left unset (defaults).
    const auto matBare = scene.nextMaterialId();
    {
        RenderMaterial m;
        m.id = matBare;
        m.name = "plain";
        scene.materials[matBare] = m;
    }

    // Root node.
    const auto rootId = scene.nextNodeId();
    scene.rootId = rootId;
    {
        RenderNode n;
        n.id = rootId;
        n.name = "root";
        n.semanticNodeId = SemanticNodeId{42};
        scene.nodes[rootId] = n;
    }
    // Child with non-identity transforms + two mesh bindings.
    const auto childId = scene.nextNodeId();
    {
        RenderNode n;
        n.id = childId;
        n.name = "child";
        n.localTransform = glm::mat4{1.f};
        n.localTransform[3] = glm::vec4{10.f, 20.f, 30.f, 1.f};
        n.localTransform[0][1] = 0.5f; // off-diagonal to catch column mix-ups
        n.worldTransform = n.localTransform;
        n.parentId = rootId;
        n.meshBindings = {
            MeshBinding{meshA, matFull},
            MeshBinding{meshB, matBare},
        };
        // A coarse hull LOD proxy — must survive the round-trip or hull LOD is a
        // no-op in WASM (proxies cross the worker→viewer boundary as bytes).
        n.lodProxyBindings = {MeshBinding{meshB, matBare}};
        n.semanticNodeId = SemanticNodeId{43};
        scene.nodes[childId] = n;
        scene.nodes[rootId].children.push_back(childId);
    }
    // Grandchild, identity transform, single binding.
    const auto grandId = scene.nextNodeId();
    {
        RenderNode n;
        n.id = grandId;
        n.name = "grand";
        n.parentId = childId;
        n.meshBindings = {MeshBinding{meshA, matBare}};
        n.semanticNodeId = SemanticNodeId{44};
        scene.nodes[grandId] = n;
        scene.nodes[childId].children.push_back(grandId);
    }

    return scene;
}

} // namespace

TEST_CASE("Render FlatBuffer roundtrip: meshes preserve geometry", "[ir][render][flatbuffer]") {
    auto original = makeScene();
    auto bytes = renderSceneToBytes(original);
    REQUIRE(!bytes.empty());
    auto restored = renderSceneFromBytes(std::as_bytes(std::span{bytes}));

    REQUIRE(restored.rootId == original.rootId);
    REQUIRE(restored.meshAssets.size() == original.meshAssets.size());

    for (const auto &[id, orig] : original.meshAssets) {
        REQUIRE(restored.meshAssets.contains(id));
        const auto &res = restored.meshAssets.at(id);
        REQUIRE(res.name == orig.name);
        REQUIRE(res.indices == orig.indices);
        REQUIRE(res.vertices.size() == orig.vertices.size());
        for (size_t i = 0; i < orig.vertices.size(); ++i) {
            REQUIRE(res.vertices[i].position.x == Approx(orig.vertices[i].position.x));
            REQUIRE(res.vertices[i].position.y == Approx(orig.vertices[i].position.y));
            REQUIRE(res.vertices[i].position.z == Approx(orig.vertices[i].position.z));
            REQUIRE(res.vertices[i].normal.x == Approx(orig.vertices[i].normal.x));
            REQUIRE(res.vertices[i].normal.y == Approx(orig.vertices[i].normal.y));
            REQUIRE(res.vertices[i].normal.z == Approx(orig.vertices[i].normal.z));
        }
        // Provenance + degradation bits survive.
        REQUIRE(res.provenance.sourceSystem == orig.provenance.sourceSystem);
        REQUIRE(res.provenance.sourceName == orig.provenance.sourceName);
        REQUIRE(res.provenance.sourceFile == orig.provenance.sourceFile);
        REQUIRE(res.provenance.degradation.bits == orig.provenance.degradation.bits);
        REQUIRE(res.provenance.degradation.has(DegradationBit::UnknownShape));
        REQUIRE(res.provenance.degradation.has(DegradationBit::TruncatedName));
        REQUIRE_FALSE(res.provenance.degradation.has(DegradationBit::MaterialMissing));

        // Stack-average prefilter hint survives (present on one mesh, nullopt on
        // the other) -- dropping it made the material prefilter a no-op in WASM.
        REQUIRE(res.stackAverage.has_value() == orig.stackAverage.has_value());
        if (orig.stackAverage.has_value()) {
            REQUIRE(res.stackAverage->avgColorLinear.x ==
                    Approx(orig.stackAverage->avgColorLinear.x));
            REQUIRE(res.stackAverage->avgColorLinear.y ==
                    Approx(orig.stackAverage->avgColorLinear.y));
            REQUIRE(res.stackAverage->avgColorLinear.z ==
                    Approx(orig.stackAverage->avgColorLinear.z));
            REQUIRE(res.stackAverage->featureSize == Approx(orig.stackAverage->featureSize));
        }
    }
}

TEST_CASE("Render FlatBuffer roundtrip: material optionals set and unset",
          "[ir][render][flatbuffer]") {
    auto original = makeScene();
    auto bytes = renderSceneToBytes(original);
    auto restored = renderSceneFromBytes(std::as_bytes(std::span{bytes}));

    REQUIRE(restored.materials.size() == original.materials.size());

    const RenderMaterial *full = nullptr;
    const RenderMaterial *bare = nullptr;
    for (const auto &[id, mat] : restored.materials) {
        if (mat.name == "glassy") {
            full = &mat;
        } else if (mat.name == "plain") {
            bare = &mat;
        }
    }
    REQUIRE(full != nullptr);
    REQUIRE(bare != nullptr);

    // Fully-populated material: every optional present with its value.
    REQUIRE(full->baseColorFactor.x == Approx(0.1f));
    REQUIRE(full->baseColorFactor.w == Approx(0.4f));
    REQUIRE(full->metallicFactor == Approx(0.7f));
    REQUIRE(full->roughnessFactor == Approx(0.3f));
    REQUIRE(full->emissiveFactor.y == Approx(0.6f));
    REQUIRE(full->alphaMode == "BLEND");
    REQUIRE(full->alphaCutoff == Approx(0.25f));
    REQUIRE(full->doubleSided);
    REQUIRE(full->ior.has_value());
    REQUIRE(*full->ior == Approx(1.5f));
    REQUIRE(full->transmissionFactor.has_value());
    REQUIRE(*full->transmissionFactor == Approx(0.9f));
    REQUIRE(full->clearcoatFactor.has_value());
    REQUIRE(*full->clearcoatFactor == Approx(0.8f));
    REQUIRE(full->clearcoatRoughnessFactor.has_value());
    REQUIRE(*full->clearcoatRoughnessFactor == Approx(0.1f));
    REQUIRE(full->anisotropyStrength.has_value());
    REQUIRE(*full->anisotropyStrength == Approx(0.2f));
    REQUIRE(full->anisotropyRotation.has_value());
    REQUIRE(*full->anisotropyRotation == Approx(0.05f));
    REQUIRE(full->specularFactor.has_value());
    REQUIRE(*full->specularFactor == Approx(0.6f));
    REQUIRE(full->specularColorFactor.has_value());
    REQUIRE(full->specularColorFactor->z == Approx(0.7f));

    // Bare material: every optional absent; scalars at their defaults.
    REQUIRE_FALSE(bare->ior.has_value());
    REQUIRE_FALSE(bare->transmissionFactor.has_value());
    REQUIRE_FALSE(bare->clearcoatFactor.has_value());
    REQUIRE_FALSE(bare->clearcoatRoughnessFactor.has_value());
    REQUIRE_FALSE(bare->anisotropyStrength.has_value());
    REQUIRE_FALSE(bare->anisotropyRotation.has_value());
    REQUIRE_FALSE(bare->specularFactor.has_value());
    REQUIRE_FALSE(bare->specularColorFactor.has_value());
    REQUIRE_FALSE(bare->doubleSided);
    REQUIRE(bare->metallicFactor == Approx(0.f));
    REQUIRE(bare->roughnessFactor == Approx(0.5f)); // schema/struct default
}

TEST_CASE("Render FlatBuffer roundtrip: hierarchy, transforms, bindings",
          "[ir][render][flatbuffer]") {
    auto original = makeScene();
    auto bytes = renderSceneToBytes(original);
    auto restored = renderSceneFromBytes(std::as_bytes(std::span{bytes}));

    REQUIRE(restored.nodes.size() == 3);

    auto byName = [&](std::string_view name) -> const RenderNode * {
        for (const auto &[id, n] : restored.nodes) {
            if (n.name == name) {
                return &n;
            }
        }
        return nullptr;
    };

    const auto *root = byName("root");
    const auto *child = byName("child");
    const auto *grand = byName("grand");
    REQUIRE(root != nullptr);
    REQUIRE(child != nullptr);
    REQUIRE(grand != nullptr);

    // Root: no parent, one child, semantic ref preserved.
    REQUIRE_FALSE(root->parentId.has_value());
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0] == child->id);
    REQUIRE(root->semanticNodeId == SemanticNodeId{42});
    REQUIRE(restored.rootId == root->id);

    // Child: parent link, transforms (column-major fidelity), two bindings.
    REQUIRE(child->parentId.has_value());
    REQUIRE(*child->parentId == root->id);
    REQUIRE(child->localTransform[3][0] == Approx(10.f));
    REQUIRE(child->localTransform[3][1] == Approx(20.f));
    REQUIRE(child->localTransform[3][2] == Approx(30.f));
    REQUIRE(child->localTransform[0][1] == Approx(0.5f));
    REQUIRE(child->worldTransform[3][2] == Approx(30.f));
    REQUIRE(child->meshBindings.size() == 2);
    REQUIRE(child->meshBindings[0].meshId.value != 0);
    REQUIRE(child->meshBindings[0].materialId.value != 0);
    // Hull LOD proxy bindings survive the round-trip.
    REQUIRE(child->lodProxyBindings.size() == 1);
    REQUIRE(child->lodProxyBindings[0].meshId.value != 0);
    REQUIRE(child->lodProxyBindings[0].materialId.value != 0);
    // Nodes without proxies stay empty (default field absent in the buffer).
    REQUIRE(grand->lodProxyBindings.empty());
    REQUIRE(child->semanticNodeId == SemanticNodeId{43});

    // Grandchild: identity transform, parent link, single binding.
    REQUIRE(grand->parentId.has_value());
    REQUIRE(*grand->parentId == child->id);
    REQUIRE(grand->localTransform[0][0] == Approx(1.f));
    REQUIRE(grand->localTransform[3][0] == Approx(0.f));
    REQUIRE(grand->meshBindings.size() == 1);
}

TEST_CASE("Render FlatBuffer: Layer 1 API composes", "[ir][render][flatbuffer]") {
    auto scene = makeScene();

    flatbuffers::FlatBufferBuilder builder{1024};
    auto offset = renderSceneToFlatBuffer(builder, scene);
    fbs::render::FinishRenderSceneBuffer(builder, offset);

    const auto *fb = fbs::render::GetRenderScene(builder.GetBufferPointer());
    auto restored = renderSceneFromFlatBuffer(*fb);

    REQUIRE(restored.nodes.size() == scene.nodes.size());
    REQUIRE(restored.meshAssets.size() == scene.meshAssets.size());
    REQUIRE(restored.rootId == scene.rootId);
}

TEST_CASE("Render FlatBuffer: file identifier check", "[ir][render][flatbuffer]") {
    auto scene = makeScene();
    auto bytes = renderSceneToBytes(scene);

    REQUIRE(bytes.size() >= 8);
    REQUIRE(flatbuffers::BufferHasIdentifier(bytes.data(), fbs::render::RenderSceneIdentifier()));

    std::vector<uint8_t> garbage = {0, 1, 2, 3, 4, 5, 6, 7};
    REQUIRE_THROWS_AS(renderSceneFromBytes(std::as_bytes(std::span{garbage})), std::runtime_error);
}

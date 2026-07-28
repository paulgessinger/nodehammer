#include <catch2/catch_test_macros.hpp>

#include <nodehammer/detail/handle_seam.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/render.hpp>

#include <memory>
#include <utility>

using namespace nodehammer;

namespace {

detail::MeshAsset makeMesh(MeshAssetId id, const char *name, std::size_t triangles) {
    detail::MeshAsset m;
    m.id = id;
    m.name = name;
    for (std::size_t t = 0; t < triangles; ++t) {
        const auto f = static_cast<float>(t);
        m.vertices.push_back(detail::Vertex{glm::vec3{f, 0.f, 0.f}, glm::vec3{0.f, 0.f, 1.f}});
        m.vertices.push_back(detail::Vertex{glm::vec3{f, 1.f, 0.f}, glm::vec3{0.f, 0.f, 1.f}});
        m.vertices.push_back(detail::Vertex{glm::vec3{f, 0.f, 1.f}, glm::vec3{0.f, 0.f, 1.f}});
        const auto base = static_cast<std::uint32_t>(t * 3);
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
    return m;
}

/// Three meshes deliberately inserted out of id order, so a test asserting
/// ascending `meshIds()` is actually asserting something.
std::shared_ptr<detail::RenderScene> makeScene() {
    auto scene = std::make_shared<detail::RenderScene>();

    const auto a = scene->nextMeshId();
    const auto b = scene->nextMeshId();
    const auto c = scene->nextMeshId();
    scene->meshAssets[c] = makeMesh(c, "third", 3);
    scene->meshAssets[a] = makeMesh(a, "first", 1);
    scene->meshAssets[b] = makeMesh(b, "second", 2);

    const auto mat = scene->nextMaterialId();
    scene->materials[mat] = RenderMaterial{.id = mat, .name = "steel"};

    const auto root = scene->nextNodeId();
    detail::RenderNode node;
    node.id = root;
    node.name = "root";
    node.meshBindings.push_back(MeshBinding{a, mat});
    scene->nodes[root] = std::move(node);
    scene->rootId = root;

    return scene;
}

} // namespace

TEST_CASE("RenderScene handle: counts mirror the underlying scene", "[api][render]") {
    auto raw = makeScene();
    const auto handle = detail::wrapRenderScene(raw);

    REQUIRE(handle.valid());
    REQUIRE(static_cast<bool>(handle));
    REQUIRE(handle.nodeCount() == raw->nodes.size());
    REQUIRE(handle.meshCount() == raw->meshAssets.size());
    REQUIRE(handle.materialCount() == raw->materials.size());
    REQUIRE(handle.rootId() == raw->rootId);

    // 1 + 2 + 3 triangles, counted per asset rather than per instance.
    REQUIRE(handle.triangleCount() == 6);
}

TEST_CASE("RenderScene handle: a default handle is inert, not a trap", "[api][render]") {
    const RenderScene handle;

    REQUIRE_FALSE(handle.valid());
    REQUIRE_FALSE(static_cast<bool>(handle));
    REQUIRE(handle.nodeCount() == 0);
    REQUIRE(handle.meshCount() == 0);
    REQUIRE(handle.triangleCount() == 0);
    REQUIRE(handle.meshIds().empty());
    REQUIRE_FALSE(handle.mesh(MeshAssetId{1}).has_value());
}

TEST_CASE("RenderScene handle: wrapping a null scene yields an invalid handle", "[api][render]") {
    const auto handle = detail::wrapRenderScene(nullptr);
    REQUIRE_FALSE(handle.valid());
}

TEST_CASE("RenderScene handle: lookup of an absent id returns nullopt", "[api][render]") {
    const auto handle = detail::wrapRenderScene(makeScene());

    REQUIRE_FALSE(handle.mesh(MeshAssetId{999999}).has_value());
}

TEST_CASE("RenderScene handle: meshIds are ascending, not container order", "[api][render]") {
    const auto handle = detail::wrapRenderScene(makeScene());
    const auto ids = handle.meshIds();

    REQUIRE(ids.size() == 3);
    REQUIRE(ids[0].value < ids[1].value);
    REQUIRE(ids[1].value < ids[2].value);

    // Same input, same order — the public order must not inherit the map's.
    REQUIRE(detail::wrapRenderScene(makeScene()).meshIds() == ids);
}

TEST_CASE("MeshView: vertex and index spans are the scene's own bytes", "[api][render]") {
    auto raw = makeScene();
    const auto handle = detail::wrapRenderScene(raw);

    for (const auto id : handle.meshIds()) {
        const auto view = handle.mesh(id);
        REQUIRE(view.has_value());

        const auto &stored = raw->meshAssets.at(id);
        // The whole argument for bindings over a subprocess: no copy at all.
        // The two Vertex types differ (public plain-float vs storage glm) but
        // are layout-identical, so address equality is the real assertion.
        REQUIRE(static_cast<const void *>(view->vertices().data()) ==
                static_cast<const void *>(stored.vertices.data()));
        REQUIRE(view->indices().data() == stored.indices.data());
        REQUIRE(view->vertexCount() == stored.vertices.size());
        REQUIRE(view->indexCount() == stored.indices.size());
        REQUIRE(view->triangleCount() == stored.indices.size() / 3);
        REQUIRE(view->name() == stored.name);
        REQUIRE(view->id() == id);
    }
}

TEST_CASE("MeshView: outlives the scene handle it came from", "[api][render]") {
    std::optional<MeshView> view;
    const detail::Vertex *expected = nullptr;
    {
        auto raw = makeScene();
        expected = raw->meshAssets.begin()->second.vertices.data();
        const auto handle = detail::wrapRenderScene(std::move(raw));
        view = handle.mesh(handle.meshIds()[0]);
        REQUIRE(view.has_value());
    }
    // Both the handle and the caller's shared_ptr are gone; the view's own
    // refcount is the only thing keeping the scene alive.
    REQUIRE(view->vertexCount() == 3);
    REQUIRE(view->scene().valid());
    (void)expected;
}

TEST_CASE("MeshView: ownedVertices keeps the scene alive by itself", "[api][render]") {
    std::shared_ptr<const Vertex[]> vertices;
    std::shared_ptr<const std::uint32_t[]> indices;
    std::size_t vertexCount = 0;

    {
        const auto handle = detail::wrapRenderScene(makeScene());
        const auto view = handle.mesh(handle.meshIds()[2]);
        REQUIRE(view.has_value());
        vertexCount = view->vertexCount();
        vertices = view->ownedVertices();
        indices = view->ownedIndices();
        // Aliasing, not copying: same address as the borrowed span.
        REQUIRE(static_cast<const void *>(vertices.get()) ==
                static_cast<const void *>(view->vertices().data()));
    }

    // Every handle and view is destroyed; the aliasing shared_ptr is all that
    // remains, and the bytes must still be readable.
    REQUIRE(vertexCount == 9);
    REQUIRE(indices[0] == 0);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        REQUIRE(vertices[i].normal.z == 1.f);
    }
}

TEST_CASE("RenderScene handle: unwrap round-trips the underlying scene", "[api][render]") {
    auto raw = makeScene();
    const auto *address = raw.get();
    const auto handle = detail::wrapRenderScene(std::move(raw));

    REQUIRE(detail::unwrapRenderScene(handle).get() == address);
    REQUIRE(detail::unwrapRenderScene(RenderScene{}) == nullptr);
}

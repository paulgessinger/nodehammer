#include <catch2/catch_test_macros.hpp>
#include <viewer/mesh_buffer_batch.hpp>

using namespace nodehammer;

TEST_CASE("GPU mesh batching preserves local indices and byte offsets for many meshes",
          "[viewer][mesh_buffer_batch]") {
    viewer::MeshBufferBatch batch;
    ir::render::MeshAsset mesh;
    mesh.indices = {2, 0, 1};
    mesh.vertices.resize(3);
    for (uint32_t i = 1; i <= 20000; ++i) {
        mesh.id = ir::render::MeshAssetId{i};
        mesh.vertices[0].position.x = static_cast<float>(i);
        REQUIRE(batch.append(mesh));
    }
    REQUIRE(batch.slices.size() == 20000);
    for (const auto &slice : batch.slices) {
        const auto v = static_cast<size_t>(slice.vertex_offset) / sizeof(ir::render::Vertex);
        const auto i = static_cast<size_t>(slice.index_offset) / sizeof(uint32_t);
        CHECK(batch.vertices[v].position.x == static_cast<float>(slice.mesh.value));
        CHECK(batch.indices[i] == 2);
        CHECK(batch.indices[i + 1] == 0);
        CHECK(batch.indices[i + 2] == 1);
    }
}

TEST_CASE("GPU mesh batches split before their byte budget without consuming the next mesh",
          "[viewer][mesh_buffer_batch]") {
    viewer::MeshBufferBatch batch;
    ir::render::MeshAsset mesh;
    mesh.id = ir::render::MeshAssetId{1};
    mesh.vertices.resize(viewer::MeshBufferBatch::target_bytes / sizeof(ir::render::Vertex));
    mesh.indices = {0, 1, 2};
    REQUIRE(batch.append(mesh)); // oversized single mesh must still make progress
    const auto vertices = batch.vertices.size();
    const auto indices = batch.indices.size();
    mesh.id = ir::render::MeshAssetId{2};
    REQUIRE_FALSE(batch.append(mesh));
    CHECK(batch.vertices.size() == vertices);
    CHECK(batch.indices.size() == indices);
    CHECK(batch.slices.size() == 1);
    viewer::MeshBufferBatch next;
    REQUIRE(next.append(mesh));
    CHECK(next.slices[0].vertex_offset == 0);
    CHECK(next.slices[0].index_offset == 0);
}

TEST_CASE("GPU mesh batching ignores empty geometry", "[viewer][mesh_buffer_batch]") {
    viewer::MeshBufferBatch batch;
    REQUIRE(batch.append(ir::render::MeshAsset{}));
    CHECK(batch.slices.empty());
    CHECK(batch.vertices.empty());
    CHECK(batch.indices.empty());
}

#pragma once

#include <ir/render.hpp>

#include <limits>
#include <stdexcept>
#include <vector>

namespace nodehammer::viewer {

// A bounded immutable upload shared by many meshes. Indices remain mesh-local:
// the draw binds both the vertex and index byte offsets for its slice.
struct MeshBufferBatch {
    static constexpr size_t target_bytes = 16 * 1024 * 1024;
    struct Slice {
        ir::render::MeshAssetId mesh;
        int vertex_offset;
        int index_offset;
    };
    std::vector<ir::render::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Slice> slices;

    bool append(const ir::render::MeshAsset &mesh) {
        if (mesh.vertices.empty() || mesh.indices.empty())
            return true;
        constexpr auto limit = static_cast<size_t>(std::numeric_limits<int>::max());
        if (mesh.vertices.size() > limit / sizeof(ir::render::Vertex) ||
            mesh.indices.size() > limit / sizeof(uint32_t))
            throw std::runtime_error("mesh exceeds GPU buffer offset limits");
        const auto vb = mesh.vertices.size() * sizeof(ir::render::Vertex);
        const auto ib = mesh.indices.size() * sizeof(uint32_t);
        const auto used_vb = vertices.size() * sizeof(ir::render::Vertex);
        const auto used_ib = indices.size() * sizeof(uint32_t);
        // A large single mesh gets its own batch, never an overflowing offset.
        if (!slices.empty() && (used_vb + used_ib + vb + ib > target_bytes))
            return false;
        slices.push_back({mesh.id, static_cast<int>(used_vb), static_cast<int>(used_ib)});
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
        return true;
    }
};

} // namespace nodehammer::viewer

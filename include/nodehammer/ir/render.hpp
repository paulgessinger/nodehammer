#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <nodehammer/ir/provenance.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nodehammer {

// ── Strong IDs ────────────────────────────────────────────────────────────────

struct RenderNodeTag {};
struct MeshAssetTag {};
struct RenderMaterialTag {};

using RenderNodeId = StrongId<RenderNodeTag>;
using MeshAssetId = StrongId<MeshAssetTag>;
using RenderMaterialId = StrongId<RenderMaterialTag>;

// ── Mesh asset (shared geometry, immutable) ───────────────────────────────────

/// Interleaved vertex: position + normal (float, local-object space)
struct Vertex {
    glm::vec3 position{0.f};
    glm::vec3 normal{0.f};
};

struct MeshAsset {
    MeshAssetId id;
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; ///< Triangles: every 3 indices = one triangle
    Provenance provenance;
};

// ── Material (per-instance binding) ──────────────────────────────────────────

struct RenderMaterial {
    RenderMaterialId id;
    std::string name;

    // PBR metallic-roughness
    glm::vec4 baseColorFactor{0.8f, 0.8f, 0.8f, 1.f}; ///< RGBA, linear
    float metallicFactor{0.f};
    float roughnessFactor{0.5f};

    // Emission
    glm::vec3 emissiveFactor{0.f};

    // Extras
    bool doubleSided{false};
};

// ── Mesh binding: material assigned to a mesh instance ───────────────────────

struct MeshBinding {
    MeshAssetId meshId;
    RenderMaterialId materialId;
};

// ── Render node ───────────────────────────────────────────────────────────────

struct RenderNode {
    RenderNodeId id;
    std::string name;

    glm::mat4 localTransform{1.f}; ///< float: from SemanticNode::localTransform
    glm::mat4 worldTransform{1.f}; ///< float: from SemanticNode::worldTransform

    std::optional<RenderNodeId> parentId;
    std::vector<RenderNodeId> children;

    /// Mesh bindings for this instance (placement-aware: material on instance, not asset)
    std::vector<MeshBinding> meshBindings;

    /// Back-reference to the semantic node that produced this render node
    SemanticNodeId semanticNodeId;
};

// ── Render scene ─────────────────────────────────────────────────────────────

struct RenderScene {
    RenderNodeId rootId;

    std::unordered_map<RenderNodeId, RenderNode> nodes;
    std::unordered_map<MeshAssetId, MeshAsset> meshAssets;
    std::unordered_map<RenderMaterialId, RenderMaterial> materials;

    // ID allocation
    RenderNodeId nextNodeId() { return RenderNodeId{nextNodeId_++}; }
    MeshAssetId nextMeshId() { return MeshAssetId{nextMeshId_++}; }
    RenderMaterialId nextMaterialId() { return RenderMaterialId{nextMaterialId_++}; }

  private:
    uint64_t nextNodeId_{1};
    uint64_t nextMeshId_{1};
    uint64_t nextMaterialId_{1};
};

// ── JSON serialization ────────────────────────────────────────────────────────

inline void to_json(nlohmann::json &j, const Vertex &v) {
    j = {
        {"position", {v.position.x, v.position.y, v.position.z}},
        {"normal", {v.normal.x, v.normal.y, v.normal.z}},
    };
}

inline void to_json(nlohmann::json &j, const MeshAsset &a) {
    j = {
        {"id", a.id},
        {"name", a.name},
        {"vertexCount", a.vertices.size()},
        {"indexCount", a.indices.size()},
        {"provenance", a.provenance},
    };
}

inline void to_json(nlohmann::json &j, const RenderMaterial &m) {
    j = {
        {"id", m.id},
        {"name", m.name},
        {"baseColorFactor", m.baseColorFactor},
        {"metallicFactor", m.metallicFactor},
        {"roughnessFactor", m.roughnessFactor},
        {"emissiveFactor", m.emissiveFactor},
        {"doubleSided", m.doubleSided},
    };
}

inline void to_json(nlohmann::json &j, const MeshBinding &b) {
    j = {{"meshId", b.meshId}, {"materialId", b.materialId}};
}

inline void to_json(nlohmann::json &j, const RenderNode &n) {
    j = {
        {"id", n.id},
        {"name", n.name},
        {"localTransform", n.localTransform},
        {"worldTransform", n.worldTransform},
        {"children", n.children},
        {"meshBindings", n.meshBindings},
        {"semanticNodeId", n.semanticNodeId},
    };
    if (n.parentId)
        j["parentId"] = *n.parentId;
}

inline void to_json(nlohmann::json &j, const RenderScene &sc) {
    auto nodes = nlohmann::json::array();
    for (const auto &[id, n] : sc.nodes)
        nodes.push_back(n);

    auto meshes = nlohmann::json::array();
    for (const auto &[id, m] : sc.meshAssets)
        meshes.push_back(m);

    auto mats = nlohmann::json::array();
    for (const auto &[id, m] : sc.materials)
        mats.push_back(m);

    j = {
        {"rootId", sc.rootId},
        {"nodes", nodes},
        {"meshAssets", meshes},
        {"materials", mats},
    };
}

} // namespace nodehammer

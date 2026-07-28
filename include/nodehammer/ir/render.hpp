#pragma once

#include <ankerl/unordered_dense.h>
#include <nlohmann/json.hpp> // RenderExtrasMap is an alias for nlohmann::json
#include <nodehammer/ir/provenance.hpp>
#include <nodehammer/ir/render_vocab.hpp>
#include <nodehammer/ir/semantic.hpp> // SemanticNodeId

// The JSON codec for these types lives in render_json.hpp / render_json.cpp so
// this header stays free of the Semantic IR's codec and cheap to include.

#include <cstdint>
#include <string>
#include <vector>

namespace nodehammer {

// ── Internal storage types ────────────────────────────────────────────────────
//
// These live in `detail` because the public API reaches them only through the
// handles in <nodehammer/render.hpp>. That is what lets their layout, container
// choice and iteration order change without breaking a consumer — and it is why
// `RenderNode::extras` (an nlohmann::json alias) can exist at all without
// dragging nlohmann into the public surface.
//
// The vocabulary above (ids, Vertex, MeshBinding, RenderMaterial) stays public:
// it appears in handle signatures, and Vertex's layout is a frozen contract
// (schemas/render.fbs) rather than an implementation detail.

namespace detail {

struct MeshAsset {
    MeshAssetId id;
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; ///< Triangles: every 3 indices = one triangle
    Provenance provenance;
    /// Set only on merged sampling-stack meshes; nullopt otherwise. See StackAverage.
    std::optional<StackAverage> stackAverage;
};

// ── Extras ────────────────────────────────────────────────────────────────────

/// Free-form metadata for export, emitted as glTF extras or similar.
using RenderExtrasMap = nlohmann::json;

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

    /// Coarse LOD proxy bindings (e.g. the convex hull of a merged sampling
    /// stack, painted with its average color). Empty for nodes without an LOD
    /// proxy. The viewer draws these instead of `meshBindings` when the stack
    /// projects small enough that the detailed geometry aliases; the two are
    /// never drawn together.
    std::vector<MeshBinding> lodProxyBindings;

    /// Back-reference to the semantic node that produced this render node
    SemanticNodeId semanticNodeId;

    /// Free-form metadata for export (e.g. glTF scene extras)
    RenderExtrasMap extras;
};

// ── Render scene ─────────────────────────────────────────────────────────────

struct RenderScene {
    RenderNodeId rootId;

    ankerl::unordered_dense::map<RenderNodeId, RenderNode> nodes;
    ankerl::unordered_dense::map<MeshAssetId, MeshAsset> meshAssets;
    ankerl::unordered_dense::map<RenderMaterialId, RenderMaterial> materials;

    // ID allocation
    RenderNodeId nextNodeId() { return RenderNodeId{nextNodeId_++}; }
    MeshAssetId nextMeshId() { return MeshAssetId{nextMeshId_++}; }
    RenderMaterialId nextMaterialId() { return RenderMaterialId{nextMaterialId_++}; }

  private:
    uint64_t nextNodeId_{1};
    uint64_t nextMeshId_{1};
    uint64_t nextMaterialId_{1};
};

} // namespace detail

} // namespace nodehammer

#pragma once

#include <ankerl/unordered_dense.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp> // RenderExtrasMap is an alias for nlohmann::json
#include <nodehammer/ir/provenance.hpp>
#include <nodehammer/ir/semantic.hpp> // StrongId, SemanticNodeId

// The JSON codec for these types lives in render_json.hpp / render_json.cpp so
// this header stays free of the Semantic IR's codec and cheap to include.

#include <cstdint>
#include <string>
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

/// Prefilter hint for a merged sampling-stack mesh (calo absorber/scintillator
/// layers etc.). Populated by the merge_descendants pass; consumed by the
/// viewer to band-limit the high-frequency cycling-material pattern that
/// aliases into moire at distance. `avgColorLinear` is the area-weighted
/// average base color over the whole stack (linear space); `featureSize` is
/// the characteristic band width (median slab thickness, world units). The
/// scene shader blends a fragment toward `avgColorLinear` once the pixel
/// footprint on the surface exceeds `featureSize` (the bands stop being
/// resolvable), converging the surface to its correct footprint-average.
struct StackAverage {
    glm::vec3 avgColorLinear{0.f};
    float featureSize{0.f};
};

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

} // namespace detail

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

    // Alpha
    std::string alphaMode{"OPAQUE"};
    float alphaCutoff{0.5f};
    // Default single-sided: tessellated detector solids are closed volumes, so
    // back-faces are hidden and culling them is correct + avoids ~2x overdraw.
    // Genuinely two-sided materials (thin shells/foils) opt in explicitly.
    bool doubleSided{false};

    // KHR extensions (nullopt = not set, omitted from glTF)
    std::optional<float> ior;
    std::optional<float> transmissionFactor;
    std::optional<float> clearcoatFactor;
    std::optional<float> clearcoatRoughnessFactor;
    std::optional<float> anisotropyStrength;
    std::optional<float> anisotropyRotation;
    std::optional<float> specularFactor;
    std::optional<glm::vec3> specularColorFactor;
};

// ── Mesh binding: material assigned to a mesh instance ───────────────────────

struct MeshBinding {
    MeshAssetId meshId;
    RenderMaterialId materialId;
};

// ── Extras ────────────────────────────────────────────────────────────────────

namespace detail {

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

#pragma once

#include <ankerl/unordered_dense.h>
#include <glm/glm.hpp>
#include <ir/provenance.hpp>
#include <ir/semantic.hpp>   // StrongId, semantic::NodeId
#include <nlohmann/json.hpp> // ExtrasMap is an alias for nlohmann::json

// The JSON codec for these types lives in render_json.hpp / render_json.cpp so
// this header stays free of the Semantic IR's codec and cheap to include.

#include <cstdint>
#include <string>
#include <vector>

namespace nodehammer::ir::render {

// ── Strong IDs ────────────────────────────────────────────────────────────────

struct NodeTag {};
struct MeshAssetTag {};
struct MaterialTag {};

using NodeId = StrongId<NodeTag>;
using MeshAssetId = StrongId<MeshAssetTag>;
using MaterialId = StrongId<MaterialTag>;

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

struct MeshAsset {
    MeshAssetId id;
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; ///< Triangles: every 3 indices = one triangle
    Provenance provenance;
    /// Set only on merged sampling-stack meshes; nullopt otherwise. See StackAverage.
    std::optional<StackAverage> stackAverage;
};

// ── Material (per-instance binding) ──────────────────────────────────────────

struct Material {
    MaterialId id;
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
    MaterialId materialId;
};

// ── Extras ────────────────────────────────────────────────────────────────────

/// Free-form metadata for export, emitted as glTF extras or similar.
using ExtrasMap = nlohmann::json;

// ── Render node ───────────────────────────────────────────────────────────────

struct Node {
    NodeId id;
    std::string name;

    glm::mat4 localTransform{1.f}; ///< float: from semantic::Node::localTransform
    glm::mat4 worldTransform{1.f}; ///< float: from semantic::Node::worldTransform

    std::optional<NodeId> parentId;
    std::vector<NodeId> children;

    /// Mesh bindings for this instance (placement-aware: material on instance, not asset)
    std::vector<MeshBinding> meshBindings;

    /// Coarse LOD proxy bindings (e.g. the convex hull of a merged sampling
    /// stack, painted with its average color). Empty for nodes without an LOD
    /// proxy. The viewer draws these instead of `meshBindings` when the stack
    /// projects small enough that the detailed geometry aliases; the two are
    /// never drawn together.
    std::vector<MeshBinding> lodProxyBindings;

    /// Back-reference to the semantic node that produced this render node
    semantic::NodeId semanticNodeId;

    /// Free-form metadata for export (e.g. glTF scene extras)
    ExtrasMap extras;
};

// ── Render scene ─────────────────────────────────────────────────────────────

struct Scene {
    NodeId rootId;

    ankerl::unordered_dense::map<NodeId, Node> nodes;
    ankerl::unordered_dense::map<MeshAssetId, MeshAsset> meshAssets;
    ankerl::unordered_dense::map<MaterialId, Material> materials;

    // ID allocation
    NodeId nextNodeId() { return NodeId{nextNodeId_++}; }
    MeshAssetId nextMeshId() { return MeshAssetId{nextMeshId_++}; }
    MaterialId nextMaterialId() { return MaterialId{nextMaterialId_++}; }

  private:
    uint64_t nextNodeId_{1};
    uint64_t nextMeshId_{1};
    uint64_t nextMaterialId_{1};
};

} // namespace nodehammer::ir::render

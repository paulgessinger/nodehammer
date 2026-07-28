#pragma once

// Render IR vocabulary: the types that appear in *signatures* rather than in
// storage. Split out of render.hpp so the public handle header
// (<nodehammer/render.hpp>) can include it without dragging in nlohmann, which
// render.hpp needs only because RenderExtrasMap aliases it.
//
// glm and unordered_dense are fine to reach here — both are shimmed by the
// amalgamated header (docs/event-display-design.md §7.3). nlohmann is not, and
// it is the one dependency the public surface must stay clear of.

#include <glm/glm.hpp>
#include <nodehammer/ir/semantic.hpp> // StrongId

#include <optional>
#include <string>

namespace nodehammer {

// ── Strong IDs ────────────────────────────────────────────────────────────────

struct RenderNodeTag {};
struct MeshAssetTag {};
struct RenderMaterialTag {};

using RenderNodeId = StrongId<RenderNodeTag>;
using MeshAssetId = StrongId<MeshAssetTag>;
using RenderMaterialId = StrongId<RenderMaterialTag>;

// ── Vertex ────────────────────────────────────────────────────────────────────

/// Interleaved vertex: position + normal (float, local-object space).
///
/// This layout is a *public contract*, not an implementation detail:
/// schemas/render.fbs pins it so the codec can memcpy whole arrays, and the
/// zero-copy mesh views in <nodehammer/render.hpp> hand out strided spans over
/// these exact bytes. Changing it is a format change.
struct Vertex {
    glm::vec3 position{0.f};
    glm::vec3 normal{0.f};
};

static_assert(sizeof(Vertex) == 24, "Vertex layout is pinned by schemas/render.fbs");
static_assert(offsetof(Vertex, normal) == 12,
              "Vertex normal offset is pinned by schemas/render.fbs");

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

} // namespace nodehammer

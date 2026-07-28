#pragma once

// Render IR vocabulary: the types that appear in public *signatures*.
//
// Deliberately minimal. StackAverage and RenderMaterial used to live here but
// no public accessor reached either of them, so they sit with the storage types
// in render.hpp now. What is left is what MeshView actually hands back.

#include <nodehammer/ir/ids.hpp>

#include <cstddef>

namespace nodehammer {

// ── Vertex ────────────────────────────────────────────────────────────────────

/// Interleaved vertex: position + normal (float, local-object space).
///
/// This layout is a *public contract*, not an implementation detail:
/// schemas/render.fbs pins it so the codec can memcpy whole arrays, and the
/// zero-copy mesh views in <nodehammer/render.hpp> hand out spans over these
/// exact bytes. Changing it is a format change.
///
/// Deliberately plain floats rather than glm::vec3, which is layout-identical.
/// Zero-copy commits to this layout whatever type expresses it, so expressing
/// it without a third-party type costs nothing and is what lets the public
/// headers be installed with no dependencies at all. The internal IR keeps glm
/// (detail::Vertex); the static_asserts below are a complete check that the two
/// agree, because a type whose layout is frozen cannot drift in any other way.
struct Vec3f {
    float x{0.f}, y{0.f}, z{0.f};
};

struct Vertex {
    Vec3f position;
    Vec3f normal;
};

static_assert(sizeof(Vertex) == 24, "Vertex layout is pinned by schemas/render.fbs");
static_assert(offsetof(Vertex, normal) == 12,
              "Vertex normal offset is pinned by schemas/render.fbs");

// ── Mesh binding: material assigned to a mesh instance ───────────────────────

struct MeshBinding {
    MeshAssetId meshId;
    RenderMaterialId materialId;
};

} // namespace nodehammer

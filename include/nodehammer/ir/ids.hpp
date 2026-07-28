#pragma once

// Strong id types, and nothing else.
//
// Split out of semantic.hpp so the public handle headers can name ids without
// pulling in the IR's storage definitions — and, with them, glm and
// unordered_dense. That is what makes installing the public headers alone
// sufficient to compile against the library.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace nodehammer {

template <typename Tag> struct StrongId {
    uint64_t value{0};

    constexpr bool operator==(const StrongId &) const noexcept = default;
    constexpr bool operator<(const StrongId &o) const noexcept { return value < o.value; }
};

struct SemanticNodeTag {};
struct SemanticLogVolTag {};
struct SemanticShapeTag {};
struct SemanticMaterialTag {};

using SemanticNodeId = StrongId<SemanticNodeTag>;
using SemanticLogVolId = StrongId<SemanticLogVolTag>;
using SemanticShapeId = StrongId<SemanticShapeTag>;
using SemanticMaterialId = StrongId<SemanticMaterialTag>;

struct RenderNodeTag {};
struct MeshAssetTag {};
struct RenderMaterialTag {};

using RenderNodeId = StrongId<RenderNodeTag>;
using MeshAssetId = StrongId<MeshAssetTag>;
using RenderMaterialId = StrongId<RenderMaterialTag>;

} // namespace nodehammer

// Hash support for StrongId
template <typename Tag> struct std::hash<nodehammer::StrongId<Tag>> {
    std::size_t operator()(const nodehammer::StrongId<Tag> &id) const noexcept {
        return std::hash<uint64_t>{}(id.value);
    }
};

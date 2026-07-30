#pragma once

#include <ir/render.hpp>

#include <flatbuffers/flatbuffers.h>
#include <render_generated.h>

#include <cstddef>
#include <span>
#include <vector>

namespace nodehammer::ir {

// FlatBuffer render IR (render::Scene) — sibling of the semantic codec.
//
// Unlike the semantic codec, this keeps a straightforward table-per-object
// layout: the render scene's weight is in mesh vertex/index arrays, which are
// serialized as flat blobs (whole-array memcpy on the hot path — see the
// static_assert in the .cpp), so the per-node/material metadata isn't worth
// column-orienting in v1. IDs are stored as full uint64, so the round-trip
// preserves original StrongId values without remapping.
//
// ── Layer 1: Type conversion (no byte I/O) ──────────────────────────────────
// Compose into a larger FlatBuffer message by taking the offset directly.

/// Serialize a render::Scene into an in-progress FlatBufferBuilder.
/// Returns the offset; the caller decides whether to Finish or nest it.
flatbuffers::Offset<fbs::render::RenderScene>
renderSceneToFlatBuffer(flatbuffers::FlatBufferBuilder &builder, const render::Scene &scene);

/// Reconstruct a render::Scene from a parsed FlatBuffer pointer. Rebuilds the
/// id-keyed maps; restores children/parent as stored.
render::Scene renderSceneFromFlatBuffer(const fbs::render::RenderScene &fb);

// ── Layer 2: Byte buffer convenience ────────────────────────────────────────

/// Serialize a render::Scene to a standalone FlatBuffer byte buffer
/// (with file identifier "NHR8").
std::vector<std::byte> renderSceneToBytes(const render::Scene &scene);

/// Deserialize a standalone FlatBuffer byte buffer to a render::Scene.
/// Verifies the buffer and file identifier before parsing.
/// Throws std::runtime_error on verification failure.
render::Scene renderSceneFromBytes(std::span<const std::byte> buf);

} // namespace nodehammer::ir

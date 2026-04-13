#pragma once

#include <nodehammer/ir/semantic.hpp>

#include <flatbuffers/flatbuffers.h>
#include <semantic_generated.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nodehammer {

// ── Layer 1: Type conversion (no byte I/O) ──────────────────────────────────
// Work with FlatBufferBuilder offsets / FlatBuffer pointers directly so callers
// can compose into larger FlatBuffer messages without intermediate buffers.

/// Serialize a SemanticScene into an in-progress FlatBufferBuilder.
/// Returns the offset; the caller decides whether to Finish or nest it.
flatbuffers::Offset<fbs::SemanticScene>
semanticSceneToFlatBuffer(flatbuffers::FlatBufferBuilder &builder, const SemanticScene &scene);

/// Reconstruct a SemanticScene from a parsed FlatBuffer pointer.
/// Does NOT call computeWorldTransforms() or computeOriginalPaths() —
/// that is the caller's responsibility (e.g. the importer).
SemanticScene semanticSceneFromFlatBuffer(const fbs::SemanticScene &fb);

// ── Layer 2: Byte buffer convenience ────────────────────────────────────────

/// Serialize a SemanticScene to a standalone FlatBuffer byte buffer
/// (with file identifier "NHSM").
std::vector<uint8_t> semanticSceneToBytes(const SemanticScene &scene);

/// Deserialize a standalone FlatBuffer byte buffer to a SemanticScene.
/// Verifies the buffer and file identifier before parsing.
/// Throws std::runtime_error on verification failure.
SemanticScene semanticSceneFromBytes(std::span<const std::byte> buf);

} // namespace nodehammer

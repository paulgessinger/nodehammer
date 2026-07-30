#pragma once

#include <ir/semantic.hpp>

#include <flatbuffers/flatbuffers.h>
#include <semantic_generated.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::ir {

// FlatBuffer semantic IR design notes:
//
// 1) Node storage is column-oriented (NodeColumns) instead of table-per-node.
//    This minimizes per-node vtable/object overhead and keeps hot columns dense.
//
// 2) Transforms are globally pooled/deduplicated (TransformPool):
//    - unique rotation rows (3x3),
//    - unique translation rows (x,y,z),
//    - transform rows reference (rotation, translation).
//    Nodes, daughters, and boolean-shape transforms reference transform rows.
//
// 3) IDs are canonicalized for compactness:
//    - node IDs are remapped to row order and ids column is omitted,
//    - logical-volume IDs are remapped to a dense local range.
//    Parent/root references are serialized in remapped space.
//
// 4) Topology is parent-only:
//    only parent_ids are stored; children vectors are reconstructed on load.
//
// 5) Width-adaptive columns:
//    log_vol_ids, name_indices, and transform_indices are written as u16 or u32
//    per file, with explicit width selectors.
//
// 6) String/index entropy reductions:
//    - name indices use frequency-ranked table assignment (common names get small IDs),
//    - source-system table assignment is frequency-ranked,
//    - tags are compacted as per-node tag_counts + flat TagRef{key,value} (u8/u8).
//
// 7) Row order is optimized for compression:
//    rows are sorted by a locality key (depth/parent/logVol/transform/name).
//    Tradeoff: node numeric IDs are not guaranteed stable across roundtrips.
//
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

struct SemanticFlatbufferSizeEntry {
    std::string label;
    std::size_t bytes{0};
};

struct SemanticFlatbufferSizeReport {
    std::size_t nodeCount{0};
    std::size_t logicalVolumeCount{0};
    std::size_t daughterPlacementCount{0};
    std::size_t shapeCount{0};
    std::size_t booleanShapeCount{0};
    std::size_t materialCount{0};
    std::size_t uniqueRotationCount{0};
    std::size_t uniqueTranslationCount{0};
    std::size_t uniqueTransformCount{0};
    bool nodeLogVolIdsUseU16{false};
    bool nodeNameIndicesUseU16{false};
    bool nodeTransformIndicesUseU16{false};
    std::size_t estimatedVectorPayloadBytes{0};
    std::vector<SemanticFlatbufferSizeEntry> entries;
};

/// Estimate major FlatBuffer payload contributors for the current schema.
/// This is intended for optimization/profiling and excludes FlatBuffers table
/// metadata overhead.
SemanticFlatbufferSizeReport semanticFlatbufferSizeReport(const SemanticScene &scene);

/// Format a human-readable size report suitable for CLI output.
std::string formatSemanticFlatbufferSizeReport(const SemanticFlatbufferSizeReport &report);

// ── Layer 2: Byte buffer convenience ────────────────────────────────────────

/// Serialize a SemanticScene to a standalone FlatBuffer byte buffer
/// (with file identifier "NHS8").
std::vector<std::byte> semanticSceneToBytes(const SemanticScene &scene);

/// Deserialize a standalone FlatBuffer byte buffer to a SemanticScene.
/// Verifies the buffer and file identifier before parsing.
/// Throws std::runtime_error on verification failure.
SemanticScene semanticSceneFromBytes(std::span<const std::byte> buf);

} // namespace nodehammer::ir

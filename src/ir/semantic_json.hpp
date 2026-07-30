#pragma once

// JSON (de)serialization for the Semantic IR. Split out of semantic.hpp so the
// data model can be included without dragging in nlohmann/json.hpp (and its
// compile-time cost) across the ~two dozen TUs that only need the types.
//
// Include this header in any TU that serializes or deserializes Semantic IR
// types via nlohmann/json. The free to_json/from_json overloads are found by
// ADL; the templated StrongId codecs live here inline, the rest are defined in
// semantic_json.cpp.

#include <detail/glm_json.hpp>
#include <ir/semantic.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace nodehammer::ir {

// ── Strong IDs (templated → header) ────────────────────────────────────────────

template <typename Tag> void to_json(nlohmann::json &j, const StrongId<Tag> &id) { j = id.value; }

template <typename Tag> void from_json(const nlohmann::json &j, StrongId<Tag> &id) {
    id.value = j.get<uint64_t>();
}

// ── Provenance / degradation ───────────────────────────────────────────────────

void to_json(nlohmann::json &j, const DegradationFlags &f);
void from_json(const nlohmann::json &j, DegradationFlags &f);
void to_json(nlohmann::json &j, const Provenance &p);
void from_json(const nlohmann::json &j, Provenance &p);

// ── Shapes ──────────────────────────────────────────────────────────────────────

void to_json(nlohmann::json &j, const BoxShape &s);
void to_json(nlohmann::json &j, const TubeShape &s);
void to_json(nlohmann::json &j, const ConeShape &s);
void to_json(nlohmann::json &j, const TrdShape &s);
void to_json(nlohmann::json &j, const ParaShape &s);
void to_json(nlohmann::json &j, const PconShape &s);
void to_json(nlohmann::json &j, const PgonShape &s);
void to_json(nlohmann::json &j, const TorusShape &s);
void to_json(nlohmann::json &j, const TessellatedShape &s);
void to_json(nlohmann::json &j, const UnknownShape &s);
void to_json(nlohmann::json &j, const BooleanUnion &s);
void to_json(nlohmann::json &j, const BooleanIntersection &s);
void to_json(nlohmann::json &j, const BooleanSubtraction &s);

void to_json(nlohmann::json &j, const SemanticShape &s);
void from_json(const nlohmann::json &j, SemanticShape &s);

/// Reconstruct the shape variant from its JSON object (dispatches on "type").
/// Unknown types decay to UnknownShape carrying the original type string.
SemanticShapeVariant shapeVariantFromJson(const nlohmann::json &j);

// ── Material / logical volume / node / scene ────────────────────────────────────

void to_json(nlohmann::json &j, const SourceMaterial &m);
void from_json(const nlohmann::json &j, SourceMaterial &m);

void to_json(nlohmann::json &j, const SemanticDaughterPlacement &d);
void from_json(const nlohmann::json &j, SemanticDaughterPlacement &d);

void to_json(nlohmann::json &j, const SemanticLogicalVolume &lv);
void from_json(const nlohmann::json &j, SemanticLogicalVolume &lv);

void to_json(nlohmann::json &j, const SemanticNode &n);
void from_json(const nlohmann::json &j, SemanticNode &n);

void to_json(nlohmann::json &j, const SemanticScene &sc);
void from_json(const nlohmann::json &j, SemanticScene &sc);

} // namespace nodehammer::ir

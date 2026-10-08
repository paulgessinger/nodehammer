#pragma once

#include <ankerl/unordered_dense.h>
#include <diagnostics.hpp>
#include <glm/glm.hpp>
#include <ir/provenance.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <queue>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace nodehammer::ir {

// ── Strong ID types ───────────────────────────────────────────────────────────
// `StrongId` itself stays in `ir`: both IRs mint ids from it, so parking it in
// either `ir::semantic` or `ir::render` would make one depend on the other.

template <typename Tag> struct StrongId {
    uint64_t value{0};

    constexpr bool operator==(const StrongId &) const noexcept = default;
    constexpr bool operator<(const StrongId &o) const noexcept { return value < o.value; }
};

} // namespace nodehammer::ir

namespace nodehammer::ir::semantic {

struct LogVolTag {};
struct ShapeTag {};
struct MaterialTag {};

using LogVolId = StrongId<LogVolTag>;
using ShapeId = StrongId<ShapeTag>;
using MaterialId = StrongId<MaterialTag>;

} // namespace nodehammer::ir::semantic

// Hash support for StrongId
template <typename Tag> struct std::hash<nodehammer::ir::StrongId<Tag>> {
    std::size_t operator()(const nodehammer::ir::StrongId<Tag> &id) const noexcept {
        return std::hash<uint64_t>{}(id.value);
    }
};

namespace nodehammer::ir::semantic {

// ── Shape types ───────────────────────────────────────────────────────────────
// All shape types must be complete before semantic::ShapeVariant is instantiated.

struct BoxShape {
    double dx{0};
    double dy{0};
    double dz{0}; ///< Half-lengths
};

struct TubeShape {
    double rMin{0};
    double rMax{0};
    double dz{0};
    double phiStart{0};
    double phiDelta{2.0 * std::numbers::pi};
};

struct ConeShape {
    double rMin1{0};
    double rMax1{0};
    double rMin2{0};
    double rMax2{0};
    double dz{0};
    double phiStart{0};
    double phiDelta{2.0 * std::numbers::pi};
};

struct TrdShape {
    double dx1{0};
    double dx2{0};
    double dy1{0};
    double dy2{0};
    double dz{0};
};

struct ParaShape {
    double dx{0};
    double dy{0};
    double dz{0};
    double alpha{0}; ///< radians
    double theta{0};
    double phi{0};
};

struct PconShape {
    double phiStart{0};
    double phiDelta{2.0 * std::numbers::pi};
    struct Section {
        double z{0};
        double rMin{0};
        double rMax{0};
    };
    std::vector<Section> sections;
};

struct PgonShape {
    double phiStart{0};
    double phiDelta{2.0 * std::numbers::pi};
    int nSides{4};
    struct Section {
        double z{0};
        double rMin{0};
        double rMax{0};
    };
    std::vector<Section> sections;
};

struct TorusShape {
    double rMin{0};
    double rMax{0};
    double rTor{0};
    double phiStart{0};
    double phiDelta{2.0 * std::numbers::pi};
};

struct TessellatedShape {
    struct Triangle {
        std::array<glm::dvec3, 3> vertices;
    };
    std::vector<Triangle> triangles;
};

struct UnknownShape {
    std::string originalType; ///< Class name from the source system
};

/// Boolean composition shapes — operands reference shapes already registered in semantic::Scene.
struct BooleanUnion {
    ShapeId left;
    ShapeId right;
    glm::dmat4 rightTransform{1.0}; ///< Transform applied to right operand
};

struct BooleanIntersection {
    ShapeId left;
    ShapeId right;
    glm::dmat4 rightTransform{1.0};
};

struct BooleanSubtraction {
    ShapeId left;
    ShapeId right;
    glm::dmat4 rightTransform{1.0};
};

using ShapeVariant = std::variant<BoxShape, TubeShape, ConeShape, TrdShape, ParaShape, PconShape,
                                  PgonShape, TorusShape, TessellatedShape, BooleanUnion,
                                  BooleanIntersection, BooleanSubtraction, UnknownShape>;

/// The boolean/CSG shape variants. These reference other shapes and must be
/// routed to the boolean tessellator; the primitive tessellator rejects them.
/// Exposed as a compile-time trait (for `if constexpr` inside std::visit) and a
/// runtime predicate over the variant, so the three-way test lives in one place.
template <typename T>
inline constexpr bool is_boolean_shape_v =
    std::is_same_v<T, BooleanUnion> || std::is_same_v<T, BooleanIntersection> ||
    std::is_same_v<T, BooleanSubtraction>;

[[nodiscard]] inline bool isBooleanShape(const ShapeVariant &shape) noexcept {
    return std::holds_alternative<BooleanUnion>(shape) ||
           std::holds_alternative<BooleanIntersection>(shape) ||
           std::holds_alternative<BooleanSubtraction>(shape);
}

struct Shape {
    ShapeId id;
    ShapeVariant data;
};

// ── Material ──────────────────────────────────────────────────────────────────

struct SourceMaterial {
    MaterialId id;
    std::string name;
    std::optional<glm::vec3> color; ///< Linear RGB, [0,1]; optional
    double density{0};              ///< g/cm³
};

// ── Logical Volume ────────────────────────────────────────────────────────────

struct DaughterPlacement {
    std::string name;
    LogVolId logVolId;
    glm::dmat4 localTransform{1.0};
};

struct LogicalVolume {
    LogicalVolume() = default;

    LogicalVolume(LogVolId id_, std::string name_, ShapeId shapeId_, MaterialId materialId_,
                  std::vector<DaughterPlacement> daughters_ = {})
        : id(id_), name(std::move(name_)), shapeId(shapeId_), materialId(materialId_),
          daughters(std::move(daughters_)) {}

    LogVolId id;
    std::string name;
    ShapeId shapeId;
    MaterialId materialId;
    /// Optional source-level daughter placements. Backends with prototype volume
    /// structure (e.g. TGeo/DD4hep) populate this; flattened importers may leave it empty.
    std::vector<DaughterPlacement> daughters;
};

// GeometryCatalogs are shared vocabulary, independent of either tree representation.
struct GeometryCatalogs {
    std::string sourceFile;
    ankerl::unordered_dense::map<LogVolId, LogicalVolume> logVols;
    ankerl::unordered_dense::map<ShapeId, Shape> shapes;
    ankerl::unordered_dense::map<MaterialId, SourceMaterial> materials;
    void reseedIdCounters();
    std::size_t deduplicateShapes();
    std::size_t deduplicateMaterials();
    std::map<LogVolId, LogVolId> deduplicateVolumeDefinitions();
    LogVolId nextLogVolId() { return LogVolId{nextLogVolId_++}; }
    ShapeId nextShapeId() { return ShapeId{nextShapeId_++}; }
    MaterialId nextMaterialId() { return MaterialId{nextMaterialId_++}; }

  private:
    uint64_t nextLogVolId_{1};
    uint64_t nextShapeId_{1};
    uint64_t nextMaterialId_{1};
};

// An identity in one immutable prototype graph: daughter indices from the root.
// Unlike a display path, this also distinguishes siblings with identical names.
using OccurrenceId = std::vector<std::size_t>;
using OccurrenceTags = std::map<std::string, std::string>;

class OccurrenceTree;
struct Occurrence;

using PlacementKey = std::pair<semantic::LogVolId, std::size_t>;

// Tags are interned explicitly; metadata maps reference zero-based tagSets entries.
// All identity keys are source daughter indices, never display-name strings.
struct Metadata {
    std::vector<OccurrenceTags> tagSets;
    std::map<semantic::LogVolId, uint64_t> volumeTags;
    std::map<PlacementKey, uint64_t> placementTags;
    std::map<OccurrenceId, uint64_t> occurrenceTags;
    std::map<OccurrenceId, std::string> displayNames;
    std::map<OccurrenceId, std::string> pathNames;
    std::map<OccurrenceId, std::string> originalPaths;
    std::map<OccurrenceId, DegradationFlags> degradation;
    std::string defaultSourceSystem;
    std::map<OccurrenceId, std::string> sourceSystems;
};

// Canonical owning, pointer-free semantic scene. GeometryCatalogs cannot contain expanded nodes.
struct Scene : GeometryCatalogs {
    semantic::DaughterPlacement root;
    Metadata metadata;

    // Throws on invalid references, source/shape cycles, non-finite transforms,
    // invalid metadata identities. Returns physical occurrence count
    // without enumerating occurrences. Zero-based tag-set IDs stay unchanged.
    [[nodiscard]] uint64_t validate() const;
    [[nodiscard]] uint64_t nodeCount() const {
        return logVols.empty() && root.logVolId.value == 0 ? 0 : validate();
    }
    std::size_t deduplicateLogVols();
    void collectGarbage();

    // Geometry must outlive the tree and all selection views, and stay immutable.
    [[nodiscard]] std::unique_ptr<semantic::OccurrenceTree>
    makeTree(std::size_t cacheCapacity = 128) const;
};

} // namespace nodehammer::ir::semantic

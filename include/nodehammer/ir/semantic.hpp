#pragma once

#include <ankerl/unordered_dense.h>
#include <glm/glm.hpp>
#include <nodehammer/ir/provenance.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <queue>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace nodehammer {

// ── Strong ID types ───────────────────────────────────────────────────────────

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

} // namespace nodehammer

// Hash support for StrongId
template <typename Tag> struct std::hash<nodehammer::StrongId<Tag>> {
    std::size_t operator()(const nodehammer::StrongId<Tag> &id) const noexcept {
        return std::hash<uint64_t>{}(id.value);
    }
};

namespace nodehammer {

// ── Shape types ───────────────────────────────────────────────────────────────
// All shape types must be complete before SemanticShapeVariant is instantiated.

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

/// Boolean composition shapes — operands reference shapes already registered in SemanticScene.
struct BooleanUnion {
    SemanticShapeId left;
    SemanticShapeId right;
    glm::dmat4 rightTransform{1.0}; ///< Transform applied to right operand
};

struct BooleanIntersection {
    SemanticShapeId left;
    SemanticShapeId right;
    glm::dmat4 rightTransform{1.0};
};

struct BooleanSubtraction {
    SemanticShapeId left;
    SemanticShapeId right;
    glm::dmat4 rightTransform{1.0};
};

using SemanticShapeVariant =
    std::variant<BoxShape, TubeShape, ConeShape, TrdShape, ParaShape, PconShape, PgonShape,
                 TorusShape, TessellatedShape, BooleanUnion, BooleanIntersection,
                 BooleanSubtraction, UnknownShape>;

/// The boolean/CSG shape variants. These reference other shapes and must be
/// routed to the boolean tessellator; the primitive tessellator rejects them.
/// Exposed as a compile-time trait (for `if constexpr` inside std::visit) and a
/// runtime predicate over the variant, so the three-way test lives in one place.
template <typename T>
inline constexpr bool is_boolean_shape_v =
    std::is_same_v<T, BooleanUnion> || std::is_same_v<T, BooleanIntersection> ||
    std::is_same_v<T, BooleanSubtraction>;

[[nodiscard]] inline bool isBooleanShape(const SemanticShapeVariant &shape) noexcept {
    return std::holds_alternative<BooleanUnion>(shape) ||
           std::holds_alternative<BooleanIntersection>(shape) ||
           std::holds_alternative<BooleanSubtraction>(shape);
}

struct SemanticShape {
    SemanticShapeId id;
    SemanticShapeVariant data;
};

// ── Material ──────────────────────────────────────────────────────────────────

struct SourceMaterial {
    SemanticMaterialId id;
    std::string name;
    std::optional<glm::vec3> color; ///< Linear RGB, [0,1]; optional
    double density{0};              ///< g/cm³
};

// ── Logical Volume ────────────────────────────────────────────────────────────

struct SemanticDaughterPlacement {
    std::string name;
    SemanticLogVolId logVolId;
    glm::dmat4 localTransform{1.0};
};

struct SemanticLogicalVolume {
    SemanticLogicalVolume() = default;

    SemanticLogicalVolume(SemanticLogVolId id_, std::string name_, SemanticShapeId shapeId_,
                          SemanticMaterialId materialId_,
                          std::vector<SemanticDaughterPlacement> daughters_ = {})
        : id(id_), name(std::move(name_)), shapeId(shapeId_), materialId(materialId_),
          daughters(std::move(daughters_)) {}

    SemanticLogVolId id;
    std::string name;
    SemanticShapeId shapeId;
    SemanticMaterialId materialId;
    /// Optional source-level daughter placements. Backends with prototype volume
    /// structure (e.g. TGeo/DD4hep) populate this; flattened importers may leave it empty.
    std::vector<SemanticDaughterPlacement> daughters;
};

// ── Node ──────────────────────────────────────────────────────────────────────

struct SemanticNode {
    SemanticNodeId id;
    std::string name;
    SemanticLogVolId logVolId;

    glm::dmat4 localTransform{1.0}; ///< Relative to parent
    glm::dmat4 worldTransform{1.0}; ///< Set by computeWorldTransforms()

    std::optional<SemanticNodeId> parentId;
    std::vector<SemanticNodeId> children;

    /// Full path in the original source tree, e.g. "/world/ODD/PixelBarrel/sensor_0".
    /// Set by computeOriginalPaths() before selection; preserved across hoisting.
    std::string originalPath;

    /// Free-form metadata tags (e.g. "subdetector"="tracker", "sensitive"="true")
    std::map<std::string, std::string> tags;

    std::string sourceSystem; ///< e.g. "dd4hep", "dd4hep/tgeo", "tgeo"
    DegradationFlags degradation;
};

// ── Scene ─────────────────────────────────────────────────────────────────────

class SemanticScene {
  public:
    SemanticNodeId rootId;
    std::string sourceFile; ///< Input file path (set by importer)

    // Flat maps indexed by ID
    ankerl::unordered_dense::map<SemanticNodeId, SemanticNode> nodes;
    ankerl::unordered_dense::map<SemanticLogVolId, SemanticLogicalVolume> logVols;
    ankerl::unordered_dense::map<SemanticShapeId, SemanticShape> shapes;
    ankerl::unordered_dense::map<SemanticMaterialId, SourceMaterial> materials;

    /// Reseed the ID allocation counters so that nextXxxId() returns values
    /// greater than every ID currently present in the maps. Deserializing
    /// importers (JSON/FlatBuffer) populate the maps directly with pre-existing
    /// IDs without advancing the counters; call this before allocating new IDs
    /// on a loaded scene to avoid colliding with — and overwriting — them.
    void reseedIdCounters();

    /// BFS pass: compose parent × local to set worldTransform on every node.
    void computeWorldTransforms();

    /// BFS pass: build originalPath from root for every reachable node.
    void computeOriginalPaths();

    /// Deduplicate shapes by value: shapes with identical parameters are merged
    /// into a single canonical entry, and all referencing logical volumes are
    /// updated.  Returns the number of shapes removed.
    std::size_t deduplicateShapes();

    /// Deduplicate logical volumes: logVols with identical (shapeId, materialId)
    /// are merged, and all referencing nodes are updated.
    /// Returns the number of logical volumes removed.
    std::size_t deduplicateLogVols();

    /// Deduplicate materials: materials with identical (name, density, color)
    /// are merged, and all referencing logical volumes are updated.
    /// Returns the number of materials removed.
    std::size_t deduplicateMaterials();

    /// BFS traversal from root; calls fn(const SemanticNode &) for every reachable node.
    /// Guards on both a missing id and a repeat visit, so a dangling child id is
    /// skipped rather than throwing and a cycle terminates rather than looping
    /// forever. Matches the guards in `reachableNodes` (src/selection/selector.cpp).
    template <typename Fn> void visitBFS(Fn &&fn) const {
        if (nodes.empty() || !nodes.contains(rootId)) {
            return;
        }
        std::unordered_set<SemanticNodeId> seen;
        seen.reserve(nodes.size());
        std::queue<SemanticNodeId> q;
        q.push(rootId);
        while (!q.empty()) {
            const auto id = q.front();
            q.pop();
            const auto it = nodes.find(id);
            if (it == nodes.end() || !seen.insert(id).second) {
                continue;
            }
            const auto &node = it->second;
            fn(node);
            for (const auto childId : node.children) {
                q.push(childId);
            }
        }
    }

    // ID allocation
    SemanticNodeId nextNodeId() { return SemanticNodeId{nextNodeId_++}; }
    SemanticLogVolId nextLogVolId() { return SemanticLogVolId{nextLogVolId_++}; }
    SemanticShapeId nextShapeId() { return SemanticShapeId{nextShapeId_++}; }
    SemanticMaterialId nextMaterialId() { return SemanticMaterialId{nextMaterialId_++}; }

  private:
    uint64_t nextNodeId_{1};
    uint64_t nextLogVolId_{1};
    uint64_t nextShapeId_{1};
    uint64_t nextMaterialId_{1};
};

} // namespace nodehammer

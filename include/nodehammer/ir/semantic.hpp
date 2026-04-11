#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>
#include <nodehammer/detail/glm_json.hpp>
#include <nodehammer/ir/provenance.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
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

struct SemanticLogicalVolume {
    SemanticLogVolId id;
    std::string name;
    SemanticShapeId shapeId;
    SemanticMaterialId materialId;
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
    std::unordered_map<SemanticNodeId, SemanticNode> nodes;
    std::unordered_map<SemanticLogVolId, SemanticLogicalVolume> logVols;
    std::unordered_map<SemanticShapeId, SemanticShape> shapes;
    std::unordered_map<SemanticMaterialId, SourceMaterial> materials;

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

    /// BFS traversal from root; calls fn(const SemanticNode &) for every reachable node.
    template <typename Fn> void visitBFS(Fn &&fn) const {
        if (nodes.empty()) {
            return;
        }
        std::queue<SemanticNodeId> q;
        q.push(rootId);
        while (!q.empty()) {
            const auto &node = nodes.at(q.front());
            q.pop();
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

// ── JSON serialization ────────────────────────────────────────────────────────

template <typename Tag> void to_json(nlohmann::json &j, const StrongId<Tag> &id) { j = id.value; }

inline void to_json(nlohmann::json &j, const DegradationFlags &f) { j = f.bits.to_ulong(); }

inline void to_json(nlohmann::json &j, const Provenance &p) {
    j = {
        {"sourceSystem", p.sourceSystem},
        {"sourceName", p.sourceName},
        {"degradation", p.degradation},
    };
    if (!p.sourceFile.empty()) {
        j["sourceFile"] = p.sourceFile;
    }
}

inline void to_json(nlohmann::json &j, const BoxShape &s) {
    j = {{"type", "box"}, {"dx", s.dx}, {"dy", s.dy}, {"dz", s.dz}};
}
inline void to_json(nlohmann::json &j, const TubeShape &s) {
    j = {{"type", "tube"}, {"rMin", s.rMin},         {"rMax", s.rMax},
         {"dz", s.dz},     {"phiStart", s.phiStart}, {"phiDelta", s.phiDelta}};
}
inline void to_json(nlohmann::json &j, const ConeShape &s) {
    j = {{"type", "cone"},         {"rMin1", s.rMin1},      {"rMax1", s.rMax1},
         {"rMin2", s.rMin2},       {"rMax2", s.rMax2},      {"dz", s.dz},
         {"phiStart", s.phiStart}, {"phiDelta", s.phiDelta}};
}
inline void to_json(nlohmann::json &j, const TrdShape &s) {
    j = {{"type", "trd"}, {"dx1", s.dx1}, {"dx2", s.dx2},
         {"dy1", s.dy1},  {"dy2", s.dy2}, {"dz", s.dz}};
}
inline void to_json(nlohmann::json &j, const ParaShape &s) {
    j = {{"type", "para"},   {"dx", s.dx},       {"dy", s.dy},  {"dz", s.dz},
         {"alpha", s.alpha}, {"theta", s.theta}, {"phi", s.phi}};
}
inline void to_json(nlohmann::json &j, const PconShape &s) {
    j = {{"type", "pcon"}, {"phiStart", s.phiStart}, {"phiDelta", s.phiDelta}};
    auto secs = nlohmann::json::array();
    for (const auto &sec : s.sections) {
        secs.push_back({{"z", sec.z}, {"rMin", sec.rMin}, {"rMax", sec.rMax}});
    }
    j["sections"] = secs;
}
inline void to_json(nlohmann::json &j, const PgonShape &s) {
    j = {
        {"type", "pgon"}, {"phiStart", s.phiStart}, {"phiDelta", s.phiDelta}, {"nSides", s.nSides}};
    auto secs = nlohmann::json::array();
    for (const auto &sec : s.sections) {
        secs.push_back({{"z", sec.z}, {"rMin", sec.rMin}, {"rMax", sec.rMax}});
    }
    j["sections"] = secs;
}
inline void to_json(nlohmann::json &j, const TorusShape &s) {
    j = {{"type", "torus"}, {"rMin", s.rMin},         {"rMax", s.rMax},
         {"rTor", s.rTor},  {"phiStart", s.phiStart}, {"phiDelta", s.phiDelta}};
}
inline void to_json(nlohmann::json &j, const TessellatedShape &s) {
    auto tris = nlohmann::json::array();
    for (const auto &tri : s.triangles) {
        tris.push_back({{tri.vertices[0].x, tri.vertices[0].y, tri.vertices[0].z},
                        {tri.vertices[1].x, tri.vertices[1].y, tri.vertices[1].z},
                        {tri.vertices[2].x, tri.vertices[2].y, tri.vertices[2].z}});
    }
    j = {{"type", "tessellated"}, {"triangles", tris}};
}
inline void to_json(nlohmann::json &j, const UnknownShape &s) {
    j = {{"type", "unknown"}, {"originalType", s.originalType}};
}
inline void to_json(nlohmann::json &j, const BooleanUnion &s) {
    j = {{"type", "union"},
         {"left", s.left},
         {"right", s.right},
         {"rightTransform", s.rightTransform}};
}
inline void to_json(nlohmann::json &j, const BooleanIntersection &s) {
    j = {{"type", "intersection"},
         {"left", s.left},
         {"right", s.right},
         {"rightTransform", s.rightTransform}};
}
inline void to_json(nlohmann::json &j, const BooleanSubtraction &s) {
    j = {{"type", "subtraction"},
         {"left", s.left},
         {"right", s.right},
         {"rightTransform", s.rightTransform}};
}

inline void to_json(nlohmann::json &j, const SemanticShape &s) {
    std::visit([&j](const auto &v) { to_json(j, v); }, s.data);
    j["id"] = s.id;
}

inline void to_json(nlohmann::json &j, const SourceMaterial &m) {
    j = {{"id", m.id}, {"name", m.name}, {"density", m.density}};
    if (m.color) {
        j["color"] = *m.color;
    }
}

inline void to_json(nlohmann::json &j, const SemanticLogicalVolume &lv) {
    j = {{"id", lv.id}, {"name", lv.name}, {"shapeId", lv.shapeId}, {"materialId", lv.materialId}};
}

inline void to_json(nlohmann::json &j, const SemanticNode &n) {
    j = {
        {"id", n.id},
        {"name", n.name},
        {"logVolId", n.logVolId},
        {"localTransform", n.localTransform},
        {"children", n.children},
        {"tags", n.tags},
        {"sourceSystem", n.sourceSystem},
        {"degradation", n.degradation},
    };
    if (n.parentId) {
        j["parentId"] = *n.parentId;
    }
}

// ── JSON deserialization ──────────────────────────────────────────────────────

template <typename Tag> void from_json(const nlohmann::json &j, StrongId<Tag> &id) {
    id.value = j.get<uint64_t>();
}

inline void from_json(const nlohmann::json &j, DegradationFlags &f) {
    f.bits = decltype(f.bits)(j.get<unsigned long>());
}

inline void from_json(const nlohmann::json &j, Provenance &p) {
    j.at("sourceSystem").get_to(p.sourceSystem);
    j.at("sourceName").get_to(p.sourceName);
    if (j.contains("sourceFile")) {
        j.at("sourceFile").get_to(p.sourceFile);
    }
    j.at("degradation").get_to(p.degradation);
}

inline SemanticShapeVariant shapeVariantFromJson(const nlohmann::json &j) {
    const auto type = j.at("type").get<std::string>();
    if (type == "box") {
        return BoxShape{j.at("dx"), j.at("dy"), j.at("dz")};
    }
    if (type == "tube") {
        return TubeShape{j.at("rMin"), j.at("rMax"), j.at("dz"), j.at("phiStart"),
                         j.at("phiDelta")};
    }
    if (type == "cone") {
        return ConeShape{j.at("rMin1"), j.at("rMax1"),    j.at("rMin2"),   j.at("rMax2"),
                         j.at("dz"),    j.at("phiStart"), j.at("phiDelta")};
    }
    if (type == "trd") {
        return TrdShape{j.at("dx1"), j.at("dx2"), j.at("dy1"), j.at("dy2"), j.at("dz")};
    }
    if (type == "para") {
        return ParaShape{j.at("dx"),    j.at("dy"),    j.at("dz"),
                         j.at("alpha"), j.at("theta"), j.at("phi")};
    }
    if (type == "pcon") {
        PconShape s;
        s.phiStart = j.at("phiStart");
        s.phiDelta = j.at("phiDelta");
        for (const auto &sec : j.at("sections")) {
            s.sections.push_back({sec.at("z"), sec.at("rMin"), sec.at("rMax")});
        }
        return s;
    }
    if (type == "pgon") {
        PgonShape s;
        s.phiStart = j.at("phiStart");
        s.phiDelta = j.at("phiDelta");
        s.nSides = j.at("nSides");
        for (const auto &sec : j.at("sections")) {
            s.sections.push_back({sec.at("z"), sec.at("rMin"), sec.at("rMax")});
        }
        return s;
    }
    if (type == "torus") {
        return TorusShape{j.at("rMin"), j.at("rMax"), j.at("rTor"), j.at("phiStart"),
                          j.at("phiDelta")};
    }
    if (type == "tessellated") {
        TessellatedShape s;
        if (j.contains("triangles")) {
            for (const auto &tri : j.at("triangles")) {
                TessellatedShape::Triangle t;
                for (int v = 0; v < 3; ++v) {
                    t.vertices[static_cast<std::size_t>(v)] = {
                        tri.at(static_cast<std::size_t>(v)).at(0).get<double>(),
                        tri.at(static_cast<std::size_t>(v)).at(1).get<double>(),
                        tri.at(static_cast<std::size_t>(v)).at(2).get<double>()};
                }
                s.triangles.push_back(t);
            }
        }
        return s;
    }
    if (type == "union") {
        BooleanUnion s;
        j.at("left").get_to(s.left);
        j.at("right").get_to(s.right);
        if (j.contains("rightTransform")) {
            j.at("rightTransform").get_to(s.rightTransform);
        }
        return s;
    }
    if (type == "intersection") {
        BooleanIntersection s;
        j.at("left").get_to(s.left);
        j.at("right").get_to(s.right);
        if (j.contains("rightTransform")) {
            j.at("rightTransform").get_to(s.rightTransform);
        }
        return s;
    }
    if (type == "subtraction") {
        BooleanSubtraction s;
        j.at("left").get_to(s.left);
        j.at("right").get_to(s.right);
        if (j.contains("rightTransform")) {
            j.at("rightTransform").get_to(s.rightTransform);
        }
        return s;
    }
    return UnknownShape{type};
}

inline void from_json(const nlohmann::json &j, SemanticShape &s) {
    j.at("id").get_to(s.id);
    s.data = shapeVariantFromJson(j);
}

inline void from_json(const nlohmann::json &j, SourceMaterial &m) {
    j.at("id").get_to(m.id);
    j.at("name").get_to(m.name);
    j.at("density").get_to(m.density);
    if (j.contains("color")) {
        m.color = j.at("color").get<glm::vec3>();
    }
}

inline void from_json(const nlohmann::json &j, SemanticLogicalVolume &lv) {
    j.at("id").get_to(lv.id);
    j.at("name").get_to(lv.name);
    j.at("shapeId").get_to(lv.shapeId);
    j.at("materialId").get_to(lv.materialId);
}

inline void from_json(const nlohmann::json &j, SemanticNode &n) {
    j.at("id").get_to(n.id);
    j.at("name").get_to(n.name);
    j.at("logVolId").get_to(n.logVolId);
    j.at("localTransform").get_to(n.localTransform);
    j.at("children").get_to(n.children);
    j.at("tags").get_to(n.tags);
    if (j.contains("sourceSystem")) {
        j.at("sourceSystem").get_to(n.sourceSystem);
    }
    if (j.contains("degradation")) {
        j.at("degradation").get_to(n.degradation);
    }
    if (j.contains("parentId")) {
        n.parentId = j.at("parentId").get<SemanticNodeId>();
    }
    // worldTransform and originalPath are recomputed after loading.
}

inline void from_json(const nlohmann::json &j, SemanticScene &sc) {
    const auto &c = j.at("content");
    c.at("rootId").get_to(sc.rootId);
    if (c.contains("sourceFile")) {
        c.at("sourceFile").get_to(sc.sourceFile);
    }
    for (const auto &jn : c.at("nodes")) {
        SemanticNode n = jn.get<SemanticNode>();
        sc.nodes[n.id] = std::move(n);
    }
    for (const auto &jlv : c.at("logVols")) {
        SemanticLogicalVolume lv = jlv.get<SemanticLogicalVolume>();
        sc.logVols[lv.id] = std::move(lv);
    }
    for (const auto &js : c.at("shapes")) {
        SemanticShape s = js.get<SemanticShape>();
        sc.shapes[s.id] = std::move(s);
    }
    for (const auto &jm : c.at("materials")) {
        SourceMaterial m = jm.get<SourceMaterial>();
        sc.materials[m.id] = std::move(m);
    }
}

// ── Scene → JSON ─────────────────────────────────────────────────────────────

inline void to_json(nlohmann::json &j, const SemanticScene &sc) {
    auto nodes = nlohmann::json::array();
    for (const auto &[id, n] : sc.nodes) {
        nodes.push_back(n);
    }

    auto logVols = nlohmann::json::array();
    for (const auto &[id, lv] : sc.logVols) {
        logVols.push_back(lv);
    }

    auto shapes = nlohmann::json::array();
    for (const auto &[id, s] : sc.shapes) {
        shapes.push_back(s);
    }

    auto mats = nlohmann::json::array();
    for (const auto &[id, m] : sc.materials) {
        mats.push_back(m);
    }

    j = {
        {"header", {{"version", 1}, {"type", "semantic"}}},
        {"content",
         {
             {"rootId", sc.rootId},
             {"sourceFile", sc.sourceFile},
             {"nodes", nodes},
             {"logVols", logVols},
             {"shapes", shapes},
             {"materials", mats},
         }},
    };
}

} // namespace nodehammer

#include <ir/semantic_json.hpp>

#include <ir/id_order.hpp>

#include <numbers>

namespace nodehammer::ir {

// ── Provenance / degradation ───────────────────────────────────────────────────

void to_json(nlohmann::json &j, const DegradationFlags &f) { j = f.bits.to_ulong(); }

void from_json(const nlohmann::json &j, DegradationFlags &f) {
    f.bits = decltype(f.bits)(j.get<unsigned long>());
}

void to_json(nlohmann::json &j, const Provenance &p) {
    j = {
        {"sourceSystem", p.sourceSystem},
        {"sourceName", p.sourceName},
        {"degradation", p.degradation},
    };
    if (!p.sourceFile.empty()) {
        j["sourceFile"] = p.sourceFile;
    }
}

void from_json(const nlohmann::json &j, Provenance &p) {
    j.at("sourceSystem").get_to(p.sourceSystem);
    j.at("sourceName").get_to(p.sourceName);
    if (j.contains("sourceFile")) {
        j.at("sourceFile").get_to(p.sourceFile);
    }
    j.at("degradation").get_to(p.degradation);
}

} // namespace nodehammer::ir

namespace nodehammer::ir::semantic {

// ── Shapes → JSON ──────────────────────────────────────────────────────────────

void to_json(nlohmann::json &j, const BoxShape &s) {
    j = {{"type", "box"}, {"dx", s.dx}, {"dy", s.dy}, {"dz", s.dz}};
}
void to_json(nlohmann::json &j, const TubeShape &s) {
    j = {{"type", "tube"}, {"rMax", s.rMax}, {"dz", s.dz}};
    if (s.rMin != 0.0) {
        j["rMin"] = s.rMin;
    }
    if (s.phiStart != 0.0) {
        j["phiStart"] = s.phiStart;
    }
    if (s.phiDelta != 2.0 * std::numbers::pi) {
        j["phiDelta"] = s.phiDelta;
    }
}
void to_json(nlohmann::json &j, const ConeShape &s) {
    j = {{"type", "cone"}, {"rMax1", s.rMax1}, {"rMax2", s.rMax2}, {"dz", s.dz}};
    if (s.rMin1 != 0.0) {
        j["rMin1"] = s.rMin1;
    }
    if (s.rMin2 != 0.0) {
        j["rMin2"] = s.rMin2;
    }
    if (s.phiStart != 0.0) {
        j["phiStart"] = s.phiStart;
    }
    if (s.phiDelta != 2.0 * std::numbers::pi) {
        j["phiDelta"] = s.phiDelta;
    }
}
void to_json(nlohmann::json &j, const TrdShape &s) {
    j = {{"type", "trd"}, {"dx1", s.dx1}, {"dx2", s.dx2},
         {"dy1", s.dy1},  {"dy2", s.dy2}, {"dz", s.dz}};
}
void to_json(nlohmann::json &j, const ParaShape &s) {
    j = {{"type", "para"},   {"dx", s.dx},       {"dy", s.dy},  {"dz", s.dz},
         {"alpha", s.alpha}, {"theta", s.theta}, {"phi", s.phi}};
}
void to_json(nlohmann::json &j, const PconShape &s) {
    j = {{"type", "pcon"}};
    if (s.phiStart != 0.0) {
        j["phiStart"] = s.phiStart;
    }
    if (s.phiDelta != 2.0 * std::numbers::pi) {
        j["phiDelta"] = s.phiDelta;
    }
    auto secs = nlohmann::json::array();
    for (const auto &sec : s.sections) {
        secs.push_back({{"z", sec.z}, {"rMin", sec.rMin}, {"rMax", sec.rMax}});
    }
    j["sections"] = secs;
}
void to_json(nlohmann::json &j, const PgonShape &s) {
    j = {{"type", "pgon"}, {"nSides", s.nSides}};
    if (s.phiStart != 0.0) {
        j["phiStart"] = s.phiStart;
    }
    if (s.phiDelta != 2.0 * std::numbers::pi) {
        j["phiDelta"] = s.phiDelta;
    }
    auto secs = nlohmann::json::array();
    for (const auto &sec : s.sections) {
        secs.push_back({{"z", sec.z}, {"rMin", sec.rMin}, {"rMax", sec.rMax}});
    }
    j["sections"] = secs;
}
void to_json(nlohmann::json &j, const TorusShape &s) {
    j = {{"type", "torus"}, {"rMax", s.rMax}, {"rTor", s.rTor}};
    if (s.rMin != 0.0) {
        j["rMin"] = s.rMin;
    }
    if (s.phiStart != 0.0) {
        j["phiStart"] = s.phiStart;
    }
    if (s.phiDelta != 2.0 * std::numbers::pi) {
        j["phiDelta"] = s.phiDelta;
    }
}
void to_json(nlohmann::json &j, const TessellatedShape &s) {
    auto tris = nlohmann::json::array();
    for (const auto &tri : s.triangles) {
        tris.push_back({{tri.vertices[0].x, tri.vertices[0].y, tri.vertices[0].z},
                        {tri.vertices[1].x, tri.vertices[1].y, tri.vertices[1].z},
                        {tri.vertices[2].x, tri.vertices[2].y, tri.vertices[2].z}});
    }
    j = {{"type", "tessellated"}, {"triangles", tris}};
}
void to_json(nlohmann::json &j, const UnknownShape &s) {
    j = {{"type", "unknown"}, {"originalType", s.originalType}};
}
void to_json(nlohmann::json &j, const BooleanUnion &s) {
    j = {{"type", "union"}, {"left", s.left}, {"right", s.right}};
    mat4ToJson(j, "rightRot", "rightTrl", s.rightTransform);
}
void to_json(nlohmann::json &j, const BooleanIntersection &s) {
    j = {{"type", "intersection"}, {"left", s.left}, {"right", s.right}};
    mat4ToJson(j, "rightRot", "rightTrl", s.rightTransform);
}
void to_json(nlohmann::json &j, const BooleanSubtraction &s) {
    j = {{"type", "subtraction"}, {"left", s.left}, {"right", s.right}};
    mat4ToJson(j, "rightRot", "rightTrl", s.rightTransform);
}

void to_json(nlohmann::json &j, const Shape &s) {
    std::visit([&j](const auto &v) { to_json(j, v); }, s.data);
    j["id"] = s.id;
}

// ── Shapes ← JSON ──────────────────────────────────────────────────────────────

ShapeVariant shapeVariantFromJson(const nlohmann::json &j) {
    const auto type = j.at("type").get<std::string>();
    if (type == "box") {
        return BoxShape{j.at("dx"), j.at("dy"), j.at("dz")};
    }
    if (type == "tube") {
        TubeShape s;
        s.rMax = j.at("rMax");
        s.dz = j.at("dz");
        if (j.contains("rMin")) {
            s.rMin = j.at("rMin");
        }
        if (j.contains("phiStart")) {
            s.phiStart = j.at("phiStart");
        }
        if (j.contains("phiDelta")) {
            s.phiDelta = j.at("phiDelta");
        }
        return s;
    }
    if (type == "cone") {
        ConeShape s;
        s.rMax1 = j.at("rMax1");
        s.rMax2 = j.at("rMax2");
        s.dz = j.at("dz");
        if (j.contains("rMin1")) {
            s.rMin1 = j.at("rMin1");
        }
        if (j.contains("rMin2")) {
            s.rMin2 = j.at("rMin2");
        }
        if (j.contains("phiStart")) {
            s.phiStart = j.at("phiStart");
        }
        if (j.contains("phiDelta")) {
            s.phiDelta = j.at("phiDelta");
        }
        return s;
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
        if (j.contains("phiStart")) {
            s.phiStart = j.at("phiStart");
        }
        if (j.contains("phiDelta")) {
            s.phiDelta = j.at("phiDelta");
        }
        for (const auto &sec : j.at("sections")) {
            s.sections.push_back({sec.at("z"), sec.at("rMin"), sec.at("rMax")});
        }
        return s;
    }
    if (type == "pgon") {
        PgonShape s;
        s.nSides = j.at("nSides");
        if (j.contains("phiStart")) {
            s.phiStart = j.at("phiStart");
        }
        if (j.contains("phiDelta")) {
            s.phiDelta = j.at("phiDelta");
        }
        for (const auto &sec : j.at("sections")) {
            s.sections.push_back({sec.at("z"), sec.at("rMin"), sec.at("rMax")});
        }
        return s;
    }
    if (type == "torus") {
        TorusShape s;
        s.rMax = j.at("rMax");
        s.rTor = j.at("rTor");
        if (j.contains("rMin")) {
            s.rMin = j.at("rMin");
        }
        if (j.contains("phiStart")) {
            s.phiStart = j.at("phiStart");
        }
        if (j.contains("phiDelta")) {
            s.phiDelta = j.at("phiDelta");
        }
        return s;
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
        mat4FromJson(j, "rightRot", "rightTrl", s.rightTransform);
        return s;
    }
    if (type == "intersection") {
        BooleanIntersection s;
        j.at("left").get_to(s.left);
        j.at("right").get_to(s.right);
        mat4FromJson(j, "rightRot", "rightTrl", s.rightTransform);
        return s;
    }
    if (type == "subtraction") {
        BooleanSubtraction s;
        j.at("left").get_to(s.left);
        j.at("right").get_to(s.right);
        mat4FromJson(j, "rightRot", "rightTrl", s.rightTransform);
        return s;
    }
    return UnknownShape{type};
}

void from_json(const nlohmann::json &j, Shape &s) {
    j.at("id").get_to(s.id);
    s.data = shapeVariantFromJson(j);
}

// ── Material / logical volume / node ────────────────────────────────────────────

void to_json(nlohmann::json &j, const SourceMaterial &m) {
    j = {{"id", m.id}, {"name", m.name}, {"density", m.density}};
    if (m.color) {
        j["color"] = *m.color;
    }
}

void from_json(const nlohmann::json &j, SourceMaterial &m) {
    j.at("id").get_to(m.id);
    j.at("name").get_to(m.name);
    j.at("density").get_to(m.density);
    if (j.contains("color")) {
        m.color = j.at("color").get<Color3>();
    }
}

void to_json(nlohmann::json &j, const DaughterPlacement &d) {
    j = {{"name", d.name}, {"logVolId", d.logVolId}};
    mat4ToJson(j, "locRot", "locTrl", d.localTransform);
}

void from_json(const nlohmann::json &j, DaughterPlacement &d) {
    j.at("name").get_to(d.name);
    j.at("logVolId").get_to(d.logVolId);
    mat4FromJson(j, "locRot", "locTrl", d.localTransform);
}

void to_json(nlohmann::json &j, const LogicalVolume &lv) {
    j = {{"id", lv.id}, {"name", lv.name}, {"shapeId", lv.shapeId}, {"materialId", lv.materialId}};
    if (!lv.daughters.empty()) {
        j["daughters"] = lv.daughters;
    }
}

void from_json(const nlohmann::json &j, LogicalVolume &lv) {
    j.at("id").get_to(lv.id);
    j.at("name").get_to(lv.name);
    j.at("shapeId").get_to(lv.shapeId);
    j.at("materialId").get_to(lv.materialId);
    if (j.contains("daughters")) {
        j.at("daughters").get_to(lv.daughters);
    }
}

void to_json(nlohmann::json &j, const Node &n) {
    j = {{"id", n.id}, {"name", n.name}, {"logVolId", n.logVolId}};
    mat4ToJson(j, "locRot", "locTrl", n.localTransform);
    if (!n.children.empty()) {
        j["children"] = n.children;
    }
    if (!n.tags.empty()) {
        j["tags"] = n.tags;
    }
    if (!n.sourceSystem.empty()) {
        j["sourceSystem"] = n.sourceSystem;
    }
    if (n.degradation.bits.any()) {
        j["degradation"] = n.degradation;
    }
    if (n.parentId) {
        j["parentId"] = *n.parentId;
    }
}

void from_json(const nlohmann::json &j, Node &n) {
    j.at("id").get_to(n.id);
    j.at("name").get_to(n.name);
    j.at("logVolId").get_to(n.logVolId);
    mat4FromJson(j, "locRot", "locTrl", n.localTransform);
    if (j.contains("children")) {
        j.at("children").get_to(n.children);
    }
    if (j.contains("tags")) {
        j.at("tags").get_to(n.tags);
    }
    if (j.contains("sourceSystem")) {
        j.at("sourceSystem").get_to(n.sourceSystem);
    }
    if (j.contains("degradation")) {
        j.at("degradation").get_to(n.degradation);
    }
    if (j.contains("parentId")) {
        n.parentId = j.at("parentId").get<NodeId>();
    }
    // worldTransform and originalPath are recomputed after loading.
}

// ── Scene ───────────────────────────────────────────────────────────────────────

void from_json(const nlohmann::json &j, Scene &sc) {
    const auto &c = j.at("content");
    c.at("rootId").get_to(sc.rootId);
    if (c.contains("sourceFile")) {
        c.at("sourceFile").get_to(sc.sourceFile);
    }
    for (const auto &jn : c.at("nodes")) {
        Node n = jn.get<Node>();
        sc.nodes[n.id] = std::move(n);
    }
    for (const auto &jlv : c.at("logVols")) {
        LogicalVolume lv = jlv.get<LogicalVolume>();
        sc.logVols[lv.id] = std::move(lv);
    }
    for (const auto &js : c.at("shapes")) {
        Shape s = js.get<Shape>();
        sc.shapes[s.id] = std::move(s);
    }
    for (const auto &jm : c.at("materials")) {
        SourceMaterial m = jm.get<SourceMaterial>();
        sc.materials[m.id] = std::move(m);
    }
}

void to_json(nlohmann::json &j, const Scene &sc) {
    // By ID, not in map order. Pruning and deduplication both erase, and the
    // IR's map erases by swapping the last element into the hole -- so without
    // this the arrays come out ordered by *what was removed*, and two dumps
    // taken either side of a one-node config change differ everywhere. See
    // ir/id_order.hpp.
    auto nodes = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.nodes)) {
        nodes.push_back(entry->second);
    }

    auto logVols = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.logVols)) {
        logVols.push_back(entry->second);
    }

    auto shapes = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.shapes)) {
        shapes.push_back(entry->second);
    }

    auto mats = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.materials)) {
        mats.push_back(entry->second);
    }

    j = {
        {"header", {{"version", 1}, {"type", "semantic"}}},
        {"content",
         {
             {"rootId", sc.rootId},
             {"nodes", nodes},
             {"logVols", logVols},
             {"shapes", shapes},
             {"materials", mats},
         }},
    };
    if (!sc.sourceFile.empty()) {
        j["content"]["sourceFile"] = sc.sourceFile;
    }
}

} // namespace nodehammer::ir::semantic

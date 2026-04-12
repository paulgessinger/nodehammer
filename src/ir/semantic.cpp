#include <nodehammer/ir/semantic.hpp>

#include <nodehammer/detail/overloaded.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace nodehammer {

namespace {

// ── Value-based shape hashing and equality ───────────────────────────────────
// We compare doubles bitwise (via bit_cast) because we want to detect shapes
// that came from the same source parameters, not approximate equality.

struct ShapeHash {
    std::size_t operator()(const SemanticShapeVariant &v) const;
};

struct ShapeEqual {
    bool operator()(const SemanticShapeVariant &a, const SemanticShapeVariant &b) const;
};

// Hash helpers
inline std::size_t hashCombine(std::size_t seed, std::size_t h) {
    return seed ^ (h + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

inline std::size_t hashDouble(double d) {
    return std::hash<uint64_t>{}(std::bit_cast<uint64_t>(d));
}

inline std::size_t hashInt(int i) { return std::hash<int>{}(i); }

inline std::size_t hashId(SemanticShapeId id) { return std::hash<uint64_t>{}(id.value); }

inline std::size_t hashMat(const glm::dmat4 &m) {
    std::size_t h = 0;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            h = hashCombine(h, hashDouble(m[c][r]));
        }
    }
    return h;
}

inline bool matEqual(const glm::dmat4 &a, const glm::dmat4 &b) {
    return std::memcmp(&a, &b, sizeof(glm::dmat4)) == 0;
}

inline std::size_t hashSections(const auto &sections) {
    std::size_t h = sections.size();
    for (const auto &s : sections) {
        h = hashCombine(h, hashDouble(s.z));
        h = hashCombine(h, hashDouble(s.rMin));
        h = hashCombine(h, hashDouble(s.rMax));
    }
    return h;
}

inline bool sectionsEqual(const auto &a, const auto &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::bit_cast<uint64_t>(a[i].z) != std::bit_cast<uint64_t>(b[i].z) ||
            std::bit_cast<uint64_t>(a[i].rMin) != std::bit_cast<uint64_t>(b[i].rMin) ||
            std::bit_cast<uint64_t>(a[i].rMax) != std::bit_cast<uint64_t>(b[i].rMax)) {
            return false;
        }
    }
    return true;
}

std::size_t ShapeHash::operator()(const SemanticShapeVariant &v) const {
    std::size_t h = std::hash<std::size_t>{}(v.index());
    return hashCombine(
        h, std::visit(
               detail::overloaded{
                   [](const BoxShape &s) {
                       return hashCombine(hashCombine(hashDouble(s.dx), hashDouble(s.dy)),
                                          hashDouble(s.dz));
                   },
                   [](const TubeShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.rMin), hashDouble(s.rMax));
                       h = hashCombine(h, hashDouble(s.dz));
                       h = hashCombine(h, hashDouble(s.phiStart));
                       return hashCombine(h, hashDouble(s.phiDelta));
                   },
                   [](const ConeShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.rMin1), hashDouble(s.rMax1));
                       h = hashCombine(h, hashDouble(s.rMin2));
                       h = hashCombine(h, hashDouble(s.rMax2));
                       h = hashCombine(h, hashDouble(s.dz));
                       h = hashCombine(h, hashDouble(s.phiStart));
                       return hashCombine(h, hashDouble(s.phiDelta));
                   },
                   [](const TrdShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.dx1), hashDouble(s.dx2));
                       h = hashCombine(h, hashDouble(s.dy1));
                       h = hashCombine(h, hashDouble(s.dy2));
                       return hashCombine(h, hashDouble(s.dz));
                   },
                   [](const ParaShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.dx), hashDouble(s.dy));
                       h = hashCombine(h, hashDouble(s.dz));
                       h = hashCombine(h, hashDouble(s.alpha));
                       h = hashCombine(h, hashDouble(s.theta));
                       return hashCombine(h, hashDouble(s.phi));
                   },
                   [](const PconShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.phiStart), hashDouble(s.phiDelta));
                       return hashCombine(h, hashSections(s.sections));
                   },
                   [](const PgonShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.phiStart), hashDouble(s.phiDelta));
                       h = hashCombine(h, hashInt(s.nSides));
                       return hashCombine(h, hashSections(s.sections));
                   },
                   [](const TorusShape &s) {
                       std::size_t h = hashCombine(hashDouble(s.rMin), hashDouble(s.rMax));
                       h = hashCombine(h, hashDouble(s.rTor));
                       h = hashCombine(h, hashDouble(s.phiStart));
                       return hashCombine(h, hashDouble(s.phiDelta));
                   },
                   [](const TessellatedShape &s) {
                       std::size_t h = s.triangles.size();
                       for (const auto &tri : s.triangles) {
                           for (const auto &v : tri.vertices) {
                               h = hashCombine(h, hashDouble(v.x));
                               h = hashCombine(h, hashDouble(v.y));
                               h = hashCombine(h, hashDouble(v.z));
                           }
                       }
                       return h;
                   },
                   [](const BooleanUnion &s) -> std::size_t {
                       return hashCombine(hashCombine(hashId(s.left), hashId(s.right)),
                                          hashMat(s.rightTransform));
                   },
                   [](const BooleanIntersection &s) -> std::size_t {
                       return hashCombine(hashCombine(hashId(s.left), hashId(s.right)),
                                          hashMat(s.rightTransform));
                   },
                   [](const BooleanSubtraction &s) -> std::size_t {
                       return hashCombine(hashCombine(hashId(s.left), hashId(s.right)),
                                          hashMat(s.rightTransform));
                   },
                   [](const UnknownShape &s) { return std::hash<std::string>{}(s.originalType); },
               },
               v));
}

// Bitwise-equal helper for doubles (exact match, not approximate).
inline bool deq(double a, double b) {
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

bool ShapeEqual::operator()(const SemanticShapeVariant &a, const SemanticShapeVariant &b) const {
    return std::visit(
        detail::overloaded{
            // Same-type cases:
            [](const BoxShape &sa, const BoxShape &sb) {
                return deq(sa.dx, sb.dx) && deq(sa.dy, sb.dy) && deq(sa.dz, sb.dz);
            },
            [](const TubeShape &sa, const TubeShape &sb) {
                return deq(sa.rMin, sb.rMin) && deq(sa.rMax, sb.rMax) && deq(sa.dz, sb.dz) &&
                       deq(sa.phiStart, sb.phiStart) && deq(sa.phiDelta, sb.phiDelta);
            },
            [](const ConeShape &sa, const ConeShape &sb) {
                return deq(sa.rMin1, sb.rMin1) && deq(sa.rMax1, sb.rMax1) &&
                       deq(sa.rMin2, sb.rMin2) && deq(sa.rMax2, sb.rMax2) && deq(sa.dz, sb.dz) &&
                       deq(sa.phiStart, sb.phiStart) && deq(sa.phiDelta, sb.phiDelta);
            },
            [](const TrdShape &sa, const TrdShape &sb) {
                return deq(sa.dx1, sb.dx1) && deq(sa.dx2, sb.dx2) && deq(sa.dy1, sb.dy1) &&
                       deq(sa.dy2, sb.dy2) && deq(sa.dz, sb.dz);
            },
            [](const ParaShape &sa, const ParaShape &sb) {
                return deq(sa.dx, sb.dx) && deq(sa.dy, sb.dy) && deq(sa.dz, sb.dz) &&
                       deq(sa.alpha, sb.alpha) && deq(sa.theta, sb.theta) && deq(sa.phi, sb.phi);
            },
            [](const PconShape &sa, const PconShape &sb) {
                return deq(sa.phiStart, sb.phiStart) && deq(sa.phiDelta, sb.phiDelta) &&
                       sectionsEqual(sa.sections, sb.sections);
            },
            [](const PgonShape &sa, const PgonShape &sb) {
                return deq(sa.phiStart, sb.phiStart) && deq(sa.phiDelta, sb.phiDelta) &&
                       sa.nSides == sb.nSides && sectionsEqual(sa.sections, sb.sections);
            },
            [](const TorusShape &sa, const TorusShape &sb) {
                return deq(sa.rMin, sb.rMin) && deq(sa.rMax, sb.rMax) && deq(sa.rTor, sb.rTor) &&
                       deq(sa.phiStart, sb.phiStart) && deq(sa.phiDelta, sb.phiDelta);
            },
            [](const TessellatedShape &sa, const TessellatedShape &sb) {
                if (sa.triangles.size() != sb.triangles.size()) {
                    return false;
                }
                return std::memcmp(sa.triangles.data(), sb.triangles.data(),
                                   sa.triangles.size() * sizeof(TessellatedShape::Triangle)) == 0;
            },
            [](const BooleanUnion &sa, const BooleanUnion &sb) {
                return sa.left == sb.left && sa.right == sb.right &&
                       matEqual(sa.rightTransform, sb.rightTransform);
            },
            [](const BooleanIntersection &sa, const BooleanIntersection &sb) {
                return sa.left == sb.left && sa.right == sb.right &&
                       matEqual(sa.rightTransform, sb.rightTransform);
            },
            [](const BooleanSubtraction &sa, const BooleanSubtraction &sb) {
                return sa.left == sb.left && sa.right == sb.right &&
                       matEqual(sa.rightTransform, sb.rightTransform);
            },
            [](const UnknownShape &sa, const UnknownShape &sb) {
                return sa.originalType == sb.originalType;
            },
            // Cross-type case: different types are never equal.
            [](const auto &, const auto &) { return false; },
        },
        a, b);
}

} // namespace

void SemanticScene::computeWorldTransforms() {
    if (nodes.empty() || !nodes.contains(rootId)) {
        return;
    }
    nodes.at(rootId).worldTransform = nodes.at(rootId).localTransform;
    visitBFS([this](const SemanticNode &node) {
        for (const auto childId : node.children) {
            auto &child = nodes.at(childId);
            child.worldTransform = node.worldTransform * child.localTransform;
        }
    });
}

void SemanticScene::computeOriginalPaths() {
    if (nodes.empty() || !nodes.contains(rootId)) {
        return;
    }
    nodes.at(rootId).originalPath = "/" + nodes.at(rootId).name;
    visitBFS([this](const SemanticNode &node) {
        for (const auto childId : node.children) {
            auto &child = nodes.at(childId);
            child.originalPath = node.originalPath + "/" + child.name;
        }
    });
}

std::size_t SemanticScene::deduplicateShapes() {
    const std::size_t before = shapes.size();
    if (before <= 1) {
        return 0;
    }

    // Phase 1: remap boolean operand IDs so that shapes referencing the same
    // canonical children hash/compare identically. We iterate in ID order
    // (parents reference children with lower IDs because dispatchTGeoShape
    // creates operands before the composite).
    // Build value → canonical ID map, processing shapes in order.
    std::unordered_map<SemanticShapeVariant, SemanticShapeId, ShapeHash, ShapeEqual> canonical;
    std::unordered_map<SemanticShapeId, SemanticShapeId> remap;

    // Collect and sort shape IDs for deterministic processing order
    std::vector<SemanticShapeId> orderedIds;
    orderedIds.reserve(shapes.size());
    for (const auto &[id, _] : shapes) {
        orderedIds.push_back(id);
    }
    std::sort(orderedIds.begin(), orderedIds.end(),
              [](SemanticShapeId a, SemanticShapeId b) { return a.value < b.value; });

    for (const SemanticShapeId id : orderedIds) {
        auto &shape = shapes.at(id);

        // Rewrite boolean operand references to canonical IDs
        std::visit(
            [&](auto &s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, BooleanUnion> ||
                              std::is_same_v<T, BooleanIntersection> ||
                              std::is_same_v<T, BooleanSubtraction>) {
                    if (auto it = remap.find(s.left); it != remap.end()) {
                        s.left = it->second;
                    }
                    if (auto it = remap.find(s.right); it != remap.end()) {
                        s.right = it->second;
                    }
                }
            },
            shape.data);

        auto [it, inserted] = canonical.try_emplace(shape.data, id);
        if (!inserted) {
            remap[id] = it->second;
        }
    }

    if (remap.empty()) {
        return 0;
    }

    // Phase 2: update all logical volumes to use canonical shape IDs
    for (auto &[_, lv] : logVols) {
        if (auto it = remap.find(lv.shapeId); it != remap.end()) {
            lv.shapeId = it->second;
        }
    }

    // Phase 3: remove duplicate shapes
    for (const auto &[dupId, _] : remap) {
        shapes.erase(dupId);
    }

    return before - shapes.size();
}

std::size_t SemanticScene::deduplicateLogVols() {
    const std::size_t before = logVols.size();
    if (before <= 1) {
        return 0;
    }

    // Key: own shape/material plus optional source-level daughter placements.
    // Backends that do not populate daughters retain the old (shapeId, materialId)
    // behavior. Backends that do populate daughters avoid collapsing containers with
    // different prototype subtrees.
    struct DaughterKey {
        uint64_t logVol;
        glm::dmat4 localTransform;

        bool operator==(const DaughterKey &o) const {
            return logVol == o.logVol && matEqual(localTransform, o.localTransform);
        }
    };
    struct Key {
        uint64_t shape;
        uint64_t material;
        std::vector<DaughterKey> daughters;

        // Defaulted == delegates to DaughterKey::operator== which uses bitwise
        // matEqual for transforms, matching the KeyHash behavior.
        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key &k) const {
            std::size_t h =
                hashCombine(std::hash<uint64_t>{}(k.shape), std::hash<uint64_t>{}(k.material));
            h = hashCombine(h, std::hash<std::size_t>{}(k.daughters.size()));
            for (const auto &d : k.daughters) {
                h = hashCombine(h, std::hash<uint64_t>{}(d.logVol));
                h = hashCombine(h, hashMat(d.localTransform));
            }
            return h;
        }
    };

    std::unordered_map<Key, SemanticLogVolId, KeyHash> canonical;
    std::unordered_map<SemanticLogVolId, SemanticLogVolId> remap;

    enum class VisitState { Visiting, Done };
    std::unordered_map<SemanticLogVolId, VisitState> states;

    auto canonicalize = [&](auto &&self, SemanticLogVolId id) -> SemanticLogVolId {
        if (auto it = remap.find(id); it != remap.end()) {
            return it->second;
        }
        auto lvIt = logVols.find(id);
        if (lvIt == logVols.end()) {
            return id;
        }
        if (auto stateIt = states.find(id); stateIt != states.end()) {
            // Logical-volume daughter graphs should be acyclic. If a backend ever
            // provides a cycle, keep the back-edge as-is rather than recursing forever.
            // Already-done nodes that were remapped are caught by the remap check above.
            return id;
        }

        states[id] = VisitState::Visiting;

        const auto &lv = lvIt->second;
        Key key{lv.shapeId.value, lv.materialId.value, {}};
        key.daughters.reserve(lv.daughters.size());
        for (const auto &daughter : lv.daughters) {
            const auto canonicalDaughter = self(self, daughter.logVolId);
            key.daughters.push_back({canonicalDaughter.value, daughter.localTransform});
        }

        const auto [it, inserted] = canonical.try_emplace(std::move(key), id);
        if (!inserted) {
            remap[id] = it->second;
        }
        states[id] = VisitState::Done;
        return inserted ? id : it->second;
    };

    std::vector<SemanticLogVolId> ids;
    ids.reserve(logVols.size());
    for (const auto &[id, _] : logVols) {
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    for (const auto id : ids) {
        canonicalize(canonicalize, id);
    }

    if (remap.empty()) {
        return 0;
    }

    // Update all nodes to use canonical logVolIds.
    for (auto &[_, node] : nodes) {
        if (auto it = remap.find(node.logVolId); it != remap.end()) {
            node.logVolId = it->second;
        }
    }

    // Update prototype daughter references as well.
    for (auto &[_, lv] : logVols) {
        for (auto &daughter : lv.daughters) {
            daughter.logVolId = canonicalize(canonicalize, daughter.logVolId);
        }
    }

    // Remove duplicates.
    for (const auto &[dupId, _] : remap) {
        logVols.erase(dupId);
    }

    return before - logVols.size();
}

std::size_t SemanticScene::deduplicateMaterials() {
    const std::size_t before = materials.size();
    if (before <= 1) {
        return 0;
    }

    struct MatKey {
        std::string name;
        uint64_t density{0};
        bool hasColor{false};
        uint32_t r{0}, g{0}, b{0};

        bool operator==(const MatKey &) const = default;
    };
    struct MatKeyHash {
        std::size_t operator()(const MatKey &k) const {
            std::size_t h = std::hash<std::string>{}(k.name);
            h = hashCombine(h, std::hash<uint64_t>{}(k.density));
            h = hashCombine(h, std::hash<bool>{}(k.hasColor));
            if (k.hasColor) {
                h = hashCombine(h, std::hash<uint32_t>{}(k.r));
                h = hashCombine(h, std::hash<uint32_t>{}(k.g));
                h = hashCombine(h, std::hash<uint32_t>{}(k.b));
            }
            return h;
        }
    };

    auto makeKey = [](const SourceMaterial &m) -> MatKey {
        MatKey k;
        k.name = m.name;
        k.density = std::bit_cast<uint64_t>(m.density);
        k.hasColor = m.color.has_value();
        if (k.hasColor) {
            k.r = std::bit_cast<uint32_t>(m.color->r);
            k.g = std::bit_cast<uint32_t>(m.color->g);
            k.b = std::bit_cast<uint32_t>(m.color->b);
        } else {
            k.r = k.g = k.b = 0;
        }
        return k;
    };

    std::unordered_map<MatKey, SemanticMaterialId, MatKeyHash> canonical;
    std::unordered_map<SemanticMaterialId, SemanticMaterialId> remap;

    // Process in ID order for determinism
    std::vector<SemanticMaterialId> ids;
    ids.reserve(materials.size());
    for (const auto &[id, _] : materials) {
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    for (const auto id : ids) {
        const auto &mat = materials.at(id);
        auto key = makeKey(mat);
        auto [it, inserted] = canonical.try_emplace(std::move(key), id);
        if (!inserted) {
            remap[id] = it->second;
        }
    }

    if (remap.empty()) {
        return 0;
    }

    // Update all logical volumes to use canonical material IDs
    for (auto &[_, lv] : logVols) {
        if (auto it = remap.find(lv.materialId); it != remap.end()) {
            lv.materialId = it->second;
        }
    }

    // Remove duplicates
    for (const auto &[dupId, _] : remap) {
        materials.erase(dupId);
    }

    return before - materials.size();
}

} // namespace nodehammer

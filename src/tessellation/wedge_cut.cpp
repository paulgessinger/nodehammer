#include <tessellation/wedge_cut.hpp>

#include <detail/overloaded.hpp>

#include <ankerl/unordered_dense.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <variant>
#include <vector>

namespace nodehammer::tessellation {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * kPi;
constexpr int kMaxBoolDepth = 32;

// ── Angle helpers (mirrors the viewer's CPU angle-cut classification) ─────────

double normAngle(double a) {
    a = std::fmod(a, kTau);
    if (a < 0.0) {
        a += kTau;
    }
    return a;
}

/// CCW span from `start` to `end`, always in (0, tau].
double sectorSpan(double start, double end) {
    const double s = normAngle(end) - normAngle(start);
    return s > 0.0 ? s : s + kTau;
}

/// Angular arc [arcLo, arcLo+arcWidth] (CCW) subtended by the xy projection of
/// an AABB, as seen from the z-axis. Returns false if the box straddles the
/// z-axis (then it covers every angle and has no finite arc). A convex region
/// not enclosing the origin spans < pi, so the four corner directions all lie on
/// a single arc < pi — found as the complement of the largest circular gap
/// between adjacent corner angles. This is exact for the box's true angular
/// extent, unlike testing whether individual corners fall inside a sector (which
/// misses a sector narrower than the box that slips between two corners).
bool aabbAngularArc(const glm::dvec3 &lo, const glm::dvec3 &hi, double &arcLo, double &arcWidth) {
    if (lo.x <= 0.0 && hi.x >= 0.0 && lo.y <= 0.0 && hi.y >= 0.0) {
        return false; // contains the z-axis → spans every angle
    }
    std::array<double, 4> a{{
        normAngle(std::atan2(lo.y, lo.x)),
        normAngle(std::atan2(lo.y, hi.x)),
        normAngle(std::atan2(hi.y, lo.x)),
        normAngle(std::atan2(hi.y, hi.x)),
    }};
    std::sort(a.begin(), a.end());
    // Largest gap between consecutive corner angles (including the wrap gap from
    // the last back to the first). The arc is everything except that gap.
    double maxGap = (a[0] + kTau) - a[3];
    std::size_t gapIdx = 3; // wrap gap sits "after" a[3]
    for (std::size_t i = 0; i < 3; ++i) {
        const double g = a[i + 1] - a[i];
        if (g > maxGap) {
            maxGap = g;
            gapIdx = i;
        }
    }
    arcLo = a[(gapIdx + 1) % 4];
    arcWidth = kTau - maxGap;
    return true;
}

/// True iff the AABB's angular arc lies entirely within the CCW removed sector
/// [start, start+removedWidth]. Conservative: a box straddling the z-axis (no
/// finite arc) is never "fully inside" a proper sector.
bool aabbArcInsideRemoved(const glm::dvec3 &lo, const glm::dvec3 &hi, double start,
                          double removedWidth) {
    double arcLo = 0.0, arcWidth = 0.0;
    if (!aabbAngularArc(lo, hi, arcLo, arcWidth)) {
        return false;
    }
    const double off = normAngle(arcLo - start); // arc start relative to sector start
    return off + arcWidth <= removedWidth + 1e-9;
}

/// True iff the AABB's angular arc is disjoint from the removed sector, i.e. it
/// lies entirely within the kept sector [start+removedWidth, start+tau].
bool aabbArcInsideKept(const glm::dvec3 &lo, const glm::dvec3 &hi, double start,
                       double removedWidth) {
    double arcLo = 0.0, arcWidth = 0.0;
    if (!aabbAngularArc(lo, hi, arcLo, arcWidth)) {
        return false;
    }
    const double keptStart = normAngle(start + removedWidth);
    const double keptWidth = kTau - removedWidth;
    const double off = normAngle(arcLo - keptStart);
    return off + arcWidth <= keptWidth + 1e-9;
}

// ── Conservative local-frame AABB per shape ───────────────────────────────────

struct Aabb {
    glm::dvec3 lo{0.0};
    glm::dvec3 hi{0.0};
    bool valid{false};
};

Aabb sym(double x, double y, double z) { return Aabb{{-x, -y, -z}, {x, y, z}, true}; }

Aabb mergeAabb(const Aabb &a, const Aabb &b) {
    if (!a.valid) {
        return b;
    }
    if (!b.valid) {
        return a;
    }
    return Aabb{glm::min(a.lo, b.lo), glm::max(a.hi, b.hi), true};
}

Aabb transformAabb(const Aabb &a, const glm::dmat4 &m) {
    if (!a.valid) {
        return a;
    }
    Aabb out;
    for (int i = 0; i < 8; ++i) {
        const glm::dvec3 corner{
            (i & 1) ? a.hi.x : a.lo.x,
            (i & 2) ? a.hi.y : a.lo.y,
            (i & 4) ? a.hi.z : a.lo.z,
        };
        const glm::dvec3 p = glm::dvec3{m * glm::dvec4{corner, 1.0}};
        if (!out.valid) {
            out.lo = out.hi = p;
            out.valid = true;
        } else {
            out.lo = glm::min(out.lo, p);
            out.hi = glm::max(out.hi, p);
        }
    }
    return out;
}

Aabb localAabb(const ir::semantic::ShapeVariant &shape, const ir::semantic::Scene &scene,
               int depth);

Aabb localAabbById(ir::semantic::ShapeId id, const ir::semantic::Scene &scene, int depth) {
    auto it = scene.shapes.find(id);
    if (it == scene.shapes.end()) {
        return Aabb{};
    }
    return localAabb(it->second.data, scene, depth);
}

Aabb localAabb(const ir::semantic::ShapeVariant &shape, const ir::semantic::Scene &scene,
               int depth) {
    if (depth > kMaxBoolDepth) {
        return Aabb{};
    }
    using detail::overloaded;
    return std::visit(
        overloaded{
            [](const ir::semantic::BoxShape &s) { return sym(s.dx, s.dy, s.dz); },
            [](const ir::semantic::TubeShape &s) { return sym(s.rMax, s.rMax, s.dz); },
            [](const ir::semantic::ConeShape &s) {
                const double r = std::max(s.rMax1, s.rMax2);
                return sym(r, r, s.dz);
            },
            [](const ir::semantic::TrdShape &s) {
                return sym(std::max(s.dx1, s.dx2), std::max(s.dy1, s.dy2), s.dz);
            },
            [](const ir::semantic::ParaShape &s) {
                const double ex = s.dx + std::abs(s.dy * std::tan(s.alpha)) +
                                  std::abs(s.dz * std::tan(s.theta) * std::cos(s.phi));
                const double ey = s.dy + std::abs(s.dz * std::tan(s.theta) * std::sin(s.phi));
                return sym(ex, ey, s.dz);
            },
            [](const ir::semantic::PconShape &s) -> Aabb {
                if (s.sections.empty()) {
                    return Aabb{};
                }
                double r = 0.0, zlo = s.sections.front().z, zhi = s.sections.front().z;
                for (const auto &sec : s.sections) {
                    r = std::max(r, sec.rMax);
                    zlo = std::min(zlo, sec.z);
                    zhi = std::max(zhi, sec.z);
                }
                return Aabb{{-r, -r, zlo}, {r, r, zhi}, true};
            },
            [](const ir::semantic::PgonShape &s) -> Aabb {
                if (s.sections.empty()) {
                    return Aabb{};
                }
                double r = 0.0, zlo = s.sections.front().z, zhi = s.sections.front().z;
                for (const auto &sec : s.sections) {
                    r = std::max(r, sec.rMax);
                    zlo = std::min(zlo, sec.z);
                    zhi = std::max(zhi, sec.z);
                }
                // rMax is the apothem (flat radius); circumscribe to be safe.
                const int n = std::max(s.nSides, 3);
                r /= std::cos(kPi / static_cast<double>(n));
                return Aabb{{-r, -r, zlo}, {r, r, zhi}, true};
            },
            [](const ir::semantic::TorusShape &s) {
                const double outer = s.rTor + s.rMax;
                return Aabb{{-outer, -outer, -s.rMax}, {outer, outer, s.rMax}, true};
            },
            [](const ir::semantic::TessellatedShape &s) -> Aabb {
                Aabb out;
                for (const auto &tri : s.triangles) {
                    for (const auto &v : tri.vertices) {
                        if (!out.valid) {
                            out.lo = out.hi = v;
                            out.valid = true;
                        } else {
                            out.lo = glm::min(out.lo, v);
                            out.hi = glm::max(out.hi, v);
                        }
                    }
                }
                return out;
            },
            [&](const ir::semantic::BooleanUnion &s) {
                const Aabb l = localAabbById(s.left, scene, depth + 1);
                const Aabb r =
                    transformAabb(localAabbById(s.right, scene, depth + 1), s.rightTransform);
                return mergeAabb(l, r);
            },
            // result ⊆ left for both subtraction and intersection
            [&](const ir::semantic::BooleanIntersection &s) {
                return localAabbById(s.left, scene, depth + 1);
            },
            [&](const ir::semantic::BooleanSubtraction &s) {
                return localAabbById(s.left, scene, depth + 1);
            },
            [](const ir::semantic::UnknownShape &) { return Aabb{}; },
        },
        shape);
}

// ── Cut deduplication key ─────────────────────────────────────────────────────
//
// Two straddling placements need the *same* cut iff they reference the same
// original shape and the wedge, transformed into their local frame, is identical.
// Because the wedge is a z-prism about the global axis with bounds chosen to
// clear all geometry, the only thing that varies is the pair of bounding
// half-planes (each through the global z-axis). We key on the original shape id
// plus a quantized signature of those two planes in the placement's local frame.
// Instances related by a translation along the cut axis (barrel z-ladders) yield
// identical signatures and therefore share one cut mesh.

/// Quantize a world-space plane transformed into a placement's local frame.
/// Planes transform by the (math) transpose of the local→world matrix.
std::array<int64_t, 4> planeSignature(const glm::dvec4 &planeWorld, const glm::dmat4 &world) {
    glm::dvec4 pl = glm::transpose(world) * planeWorld;
    const double len = glm::length(glm::dvec3{pl});
    if (len > 0.0) {
        pl /= len;
    }
    return {std::llround(pl.x * 1e6), std::llround(pl.y * 1e6), std::llround(pl.z * 1e6),
            std::llround(pl.w * 1e3)};
}

struct CutKey {
    uint64_t shapeId{0};
    std::array<int64_t, 4> planeA{};
    std::array<int64_t, 4> planeB{};
    bool operator==(const CutKey &) const = default;
};

struct CutKeyHash {
    std::size_t operator()(const CutKey &k) const noexcept {
        std::size_t h = std::hash<uint64_t>{}(k.shapeId);
        auto mix = [&h](int64_t v) {
            h ^= std::hash<uint64_t>{}(static_cast<uint64_t>(v)) + 0x9e3779b97f4a7c15ULL +
                 (h << 6) + (h >> 2);
        };
        for (int64_t v : k.planeA) {
            mix(v);
        }
        for (int64_t v : k.planeB) {
            mix(v);
        }
        return h;
    }
};

} // namespace

// ── Cooperative wedge-cut job ─────────────────────────────────────────────────
//
// The original single-shot pass is split into three resumable phases so the
// caller can spread the work across `advance()` calls:
//   * Bounds   — sweep every placement to size the cutting solid (pass 1);
//   * Classify — sweep every placement, rewriting its logVol to empty / cut /
//                kept and building cut shapes on demand (pass 2);
//   * Prune    — drop fully-cut-away subtrees (pass 3), run as one final slice.
// Progress (`processed` / `total`) tracks the classify sweep, the dominant and
// user-meaningful "applying the cut to N placements" work.

struct WedgeCutJob::Impl {
    enum class Phase : std::uint8_t { Idle, Bounds, Classify, Prune };

    ir::semantic::Scene *scene{nullptr};

    double startRad{0.0};
    double endRad{0.0};
    double removedWidth{0.0};
    double margin{2.0};

    // Stable placement snapshot taken at start: Bounds and Classify only mutate
    // node *values* and insert into the shapes/logVols maps (never scene.nodes),
    // so iterating this id list across advance() calls stays valid. Prune is the
    // only phase that erases nodes, and it runs after the snapshot is consumed.
    std::vector<ir::semantic::NodeId> nodeIds;
    std::size_t idx{0};
    Phase phase{Phase::Idle};
    bool started{false};
    bool done{false};

    // Pass-1 accumulators.
    double maxR{0.0};
    double maxZ{0.0};
    bool anyBounds{false};

    // Built once Bounds completes, consumed during Classify.
    ir::semantic::ShapeId wedgeId{};
    ir::semantic::LogVolId emptyLvId{};
    glm::dvec4 planeStart{};
    glm::dvec4 planeEnd{};

    // Cut-shape dedup cache: identical (shape, local-frame wedge) → shared cut
    // logVol, so instances that need the same cut stay instanced.
    ankerl::unordered_dense::map<CutKey, ir::semantic::LogVolId, CutKeyHash> cutCache;
    // Per-node "produces its own geometry" flag (kept or cut), used by Prune.
    // Emptied/skipped nodes produce no mesh; unclassified nodes default to "has
    // geometry" (never pruned).
    ankerl::unordered_dense::map<ir::semantic::NodeId, bool> hasOwnGeom;

    WedgeCutStats stats;
    // Counters are atomic so the SceneBuildJob's native worker thread can bump
    // `processed` while the main thread reads it for the UI bar (relaxed — the
    // bar tolerates a frame of staleness).
    std::atomic<std::size_t> total{0};
    std::atomic<std::size_t> processed{0};

    // ── Pass 1 (per node): grow the global radius/z bounds. ──────────────────
    void stepBounds(ir::semantic::NodeId id) {
        const auto nit = scene->nodes.find(id);
        if (nit == scene->nodes.end()) {
            return;
        }
        const ir::semantic::Node &node = nit->second;
        const auto lvIt = scene->logVols.find(node.logVolId);
        if (lvIt == scene->logVols.end()) {
            return;
        }
        const Aabb world =
            transformAabb(localAabbById(lvIt->second.shapeId, *scene, 0), node.worldTransform);
        if (!world.valid) {
            return;
        }
        for (int i = 0; i < 4; ++i) {
            const double x = (i & 1) ? world.hi.x : world.lo.x;
            const double y = (i & 2) ? world.hi.y : world.lo.y;
            maxR = std::max(maxR, std::hypot(x, y));
        }
        maxZ = std::max({maxZ, std::abs(world.lo.z), std::abs(world.hi.z)});
        anyBounds = true;
    }

    // Build the shared cutting solid + empty shape once bounds are known, then
    // advance into the classify phase (or finish if there was no geometry).
    void finishBounds() {
        if (!anyBounds) {
            done = true; // nothing bounded → leave scene untouched
            return;
        }
        const double m = std::max(margin, 1.0);
        const double cutR = std::max(m * maxR, 1.0);
        const double cutZ = std::max(m * maxZ, 1.0);

        // Shared cutting solid: a phi-sector tube covering the removed wedge.
        wedgeId = scene->nextShapeId();
        scene->shapes[wedgeId] = ir::semantic::Shape{
            wedgeId, ir::semantic::TubeShape{0.0, cutR, cutZ, startRad, removedWidth}};

        // Shared empty shape for placements fully inside the removed sector.
        const ir::semantic::ShapeId emptyShapeId = scene->nextShapeId();
        scene->shapes[emptyShapeId] =
            ir::semantic::Shape{emptyShapeId, ir::semantic::TessellatedShape{}};
        const ir::semantic::MaterialId fillerMat =
            scene->materials.empty() ? ir::semantic::MaterialId{} : scene->materials.begin()->first;
        emptyLvId = scene->nextLogVolId();
        scene->logVols[emptyLvId] = ir::semantic::LogicalVolume{
            emptyLvId, std::string{kWedgeEmptyLogVolName}, emptyShapeId, fillerMat};

        // The two bounding half-planes of the removed sector, through the global
        // z-axis (normal in xy, d = 0). Used to build per-placement cut sigs.
        planeStart = glm::dvec4{-std::sin(startRad), std::cos(startRad), 0.0, 0.0};
        planeEnd = glm::dvec4{-std::sin(endRad), std::cos(endRad), 0.0, 0.0};

        phase = Phase::Classify;
        idx = 0;
    }

    // ── Pass 2 (per node): classify and rewrite the placement's logVol. ──────
    // We only mutate scene.nodes values (logVolId) in place and *insert* into
    // scene.shapes / scene.logVols. Fields read from the (reference-returning)
    // maps are copied out before any insert that could rehash them.
    void stepClassify(ir::semantic::NodeId id) {
        const auto nit = scene->nodes.find(id);
        if (nit == scene->nodes.end()) {
            return;
        }
        ir::semantic::Node &node = nit->second;
        const auto lvIt = scene->logVols.find(node.logVolId);
        if (lvIt == scene->logVols.end()) {
            return;
        }
        const ir::semantic::ShapeId origShapeId = lvIt->second.shapeId;
        const ir::semantic::MaterialId origMat = lvIt->second.materialId;
        const std::string lvName = lvIt->second.name;

        const Aabb local = localAabbById(origShapeId, *scene, 0);
        if (!local.valid) {
            ++stats.skipped;
            hasOwnGeom[id] = false; // unbounded shapes produce no tessellatable mesh
            return;
        }
        const Aabb world = transformAabb(local, node.worldTransform);

        if (aabbArcInsideRemoved(world.lo, world.hi, startRad, removedWidth)) {
            // Fully inside the removed sector → render nothing.
            node.logVolId = emptyLvId;
            ++stats.emptied;
            hasOwnGeom[id] = false;
        } else if (aabbArcInsideKept(world.lo, world.hi, startRad, removedWidth)) {
            // Fully inside the kept sector → leave shared (instanced).
            ++stats.kept;
            hasOwnGeom[id] = true;
        } else {
            // Straddles the boundary → boolean-subtraction cut. Reuse an existing
            // cut shape when this placement's (shape, local-frame wedge) matches
            // one already created, so instanced copies needing the same cut stay
            // instanced; otherwise create a fresh cut shape in this local frame.
            const CutKey key{origShapeId.value, planeSignature(planeStart, node.worldTransform),
                             planeSignature(planeEnd, node.worldTransform)};
            ++stats.cut;
            if (auto it = cutCache.find(key); it != cutCache.end()) {
                node.logVolId = it->second;
            } else {
                const ir::semantic::ShapeId cutShapeId = scene->nextShapeId();
                scene->shapes[cutShapeId] = ir::semantic::Shape{
                    cutShapeId, ir::semantic::BooleanSubtraction{
                                    origShapeId, wedgeId, glm::inverse(node.worldTransform)}};
                const ir::semantic::LogVolId cutLvId = scene->nextLogVolId();
                scene->logVols[cutLvId] =
                    ir::semantic::LogicalVolume{cutLvId, lvName + "_wedgecut", cutShapeId, origMat};
                cutCache.emplace(key, cutLvId);
                node.logVolId = cutLvId;
                ++stats.cutUnique;
            }
            hasOwnGeom[id] = true;
        }
    }

    // ── Pass 3: prune fully-cut-away subtrees ────────────────────────────────
    // A subtree that contains no kept/cut node produces nothing; removing it
    // keeps the render tree clean and, importantly, prevents merge_descendants
    // parents (e.g. a stave entirely inside the removed wedge) from being asked
    // to merge an all-empty set of children (which would emit NH0505).
    void runPrune() {
        if (!scene->nodes.contains(scene->rootId)) {
            return;
        }
        ankerl::unordered_dense::map<ir::semantic::NodeId, bool> subtreeGeom;
        auto computeGeom = [&](auto &&self, ir::semantic::NodeId nid) -> bool {
            const auto it = scene->nodes.find(nid);
            if (it == scene->nodes.end()) {
                return false;
            }
            // Default to "has geometry" for nodes we never classified (e.g. logVol
            // missing) so we never prune something we don't understand.
            bool g = hasOwnGeom.contains(nid) ? hasOwnGeom.at(nid) : true;
            for (const auto childId : it->second.children) {
                g = self(self, childId) || g;
            }
            subtreeGeom[nid] = g;
            return g;
        };
        computeGeom(computeGeom, scene->rootId);

        // Collect maximal empty subtree roots: an empty node whose parent keeps
        // geometry. (The root itself is never removed.)
        std::vector<ir::semantic::NodeId> removalRoots;
        for (const auto &[id, geom] : subtreeGeom) {
            if (geom || id == scene->rootId) {
                continue;
            }
            const auto nit = scene->nodes.find(id);
            if (nit == scene->nodes.end() || !nit->second.parentId) {
                continue;
            }
            const auto pit = subtreeGeom.find(*nit->second.parentId);
            if (pit != subtreeGeom.end() && pit->second) {
                removalRoots.push_back(id);
            }
        }

        for (const auto rootOfEmpty : removalRoots) {
            const auto nit = scene->nodes.find(rootOfEmpty);
            if (nit == scene->nodes.end()) {
                continue;
            }
            // Detach from parent.
            if (nit->second.parentId) {
                auto pit = scene->nodes.find(*nit->second.parentId);
                if (pit != scene->nodes.end()) {
                    auto &kids = pit->second.children;
                    kids.erase(std::remove(kids.begin(), kids.end(), rootOfEmpty), kids.end());
                }
            }
            // Erase the whole subtree.
            std::vector<ir::semantic::NodeId> stack{rootOfEmpty};
            while (!stack.empty()) {
                const auto cur = stack.back();
                stack.pop_back();
                const auto cit = scene->nodes.find(cur);
                if (cit == scene->nodes.end()) {
                    continue;
                }
                for (const auto childId : cit->second.children) {
                    stack.push_back(childId);
                }
                scene->nodes.erase(cit);
                ++stats.pruned;
            }
        }
    }
};

WedgeCutJob::WedgeCutJob() : impl_(std::make_unique<Impl>()) {}
WedgeCutJob::~WedgeCutJob() = default;
WedgeCutJob::WedgeCutJob(WedgeCutJob &&) noexcept = default;
WedgeCutJob &WedgeCutJob::operator=(WedgeCutJob &&) noexcept = default;

void WedgeCutJob::start(ir::semantic::Scene &scene, const WedgeCutParams &params) {
    // std::atomic members make Impl non-assignable; replace the unique_ptr
    // wholesale to reset the job between runs.
    impl_ = std::make_unique<Impl>();
    Impl &im = *impl_;
    im.scene = &scene;
    im.startRad = params.startDeg * kPi / 180.0;
    im.endRad = params.endDeg * kPi / 180.0;
    im.removedWidth = sectorSpan(im.startRad, im.endRad);
    im.margin = params.margin;
    im.started = true;

    // Degenerate sector: nothing (or everything) removed → leave scene untouched.
    if (im.removedWidth < 1e-6 || im.removedWidth > kTau - 1e-6) {
        im.done = true;
        return;
    }

    scene.computeWorldTransforms();
    // Loaded scenes carry stale ID counters; ensure our new shapes/logVols get
    // fresh IDs rather than overwriting existing entries.
    scene.reseedIdCounters();

    im.nodeIds.reserve(scene.nodes.size());
    for (const auto &[id, node] : scene.nodes) {
        (void)node;
        im.nodeIds.push_back(id);
    }
    im.total.store(im.nodeIds.size(), std::memory_order_relaxed);
    im.phase = Impl::Phase::Bounds;
    im.idx = 0;
}

bool WedgeCutJob::advance(std::uint64_t budget_ns) {
    Impl &im = *impl_;
    if (!im.started) {
        im.done = true;
        return true;
    }
    if (im.done) {
        return true;
    }
    // See TessellationJob::advance: batch the wall-clock sampling so a coarsened
    // Web Worker clock can't collapse the slice to one step per advance() call
    // and flood a per-slice progress emitter.
    constexpr std::uint64_t kClockCheckStride = 128;
    const auto start_time = std::chrono::steady_clock::now();
    std::uint64_t since_check = 0;
    while (!im.done) {
        switch (im.phase) {
        case Impl::Phase::Bounds:
            if (im.idx >= im.nodeIds.size()) {
                im.finishBounds(); // builds the cutter + flips to Classify, or finishes
            } else {
                im.stepBounds(im.nodeIds[im.idx]);
                ++im.idx;
            }
            break;
        case Impl::Phase::Classify:
            if (im.idx >= im.nodeIds.size()) {
                im.phase = Impl::Phase::Prune;
            } else {
                im.stepClassify(im.nodeIds[im.idx]);
                ++im.idx;
                im.processed.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case Impl::Phase::Prune:
            im.runPrune(); // single slice — may exceed the budget, like a big node
            im.done = true;
            break;
        case Impl::Phase::Idle:
            im.done = true;
            break;
        }
        if (im.done) {
            break;
        }
        if (++since_check < kClockCheckStride) {
            continue;
        }
        since_check = 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start_time)
                                 .count();
        if (static_cast<std::uint64_t>(elapsed) >= budget_ns) {
            break;
        }
    }
    return im.done;
}

WedgeCutStats WedgeCutJob::take() { return impl_->stats; }

std::size_t WedgeCutJob::totalPlacements() const {
    return impl_->total.load(std::memory_order_relaxed);
}
std::size_t WedgeCutJob::processedPlacements() const {
    return impl_->processed.load(std::memory_order_relaxed);
}

// ── applyWedgeCut (run-to-completion shim) ────────────────────────────────────

WedgeCutStats applyWedgeCut(ir::semantic::Scene &scene, const WedgeCutParams &params) {
    WedgeCutJob job;
    job.start(scene, params);
    while (!job.advance(std::numeric_limits<std::uint64_t>::max())) {
    }
    return job.take();
}

} // namespace nodehammer::tessellation

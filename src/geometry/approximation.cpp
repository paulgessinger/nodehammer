#include <geometry/approximation.hpp>

#include <diagnostic_codes.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <selection/predicate.hpp>
#include <tessellation/boolean_tessellator.hpp>
#include <tessellation/primitive_tessellator.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>

namespace nodehammer::geometry {
namespace {
using namespace ir::semantic;

struct Rule {
    const config::Rule *source;
    selection::Predicate matches;
};
struct Settings {
    bool replace{};
    bool skip{};
    tessellation::TessellationParams tess;
};

// Removing daughters does not renumber any surviving route. Only metadata
// strictly below a replaced node becomes invalid.
template <class Map> void eraseDescendants(Map &map, const OccurrenceId &id) {
    auto it = map.upper_bound(id);
    while (it != map.end() && it->first.size() > id.size() &&
           std::equal(id.begin(), id.end(), it->first.begin()))
        it = map.erase(it);
}

class Approximation {
  public:
    Approximation(const Scene &source, const config::NHConfig &config)
        : source_(source), config_(config), tree_(source.makeTree(0)), result_(source) {
        result_.reseedIdCounters();
        for (const auto &rule : config.rules)
            rules_.push_back(
                {&rule, rule.match ? selection::compilePredicate(*rule.match)
                                   : selection::Predicate{[](const auto &) { return true; }}});
    }

    DiagnosticList run(Scene &destination) {
        OccurrenceId route;
        result_.root.logVolId = visit(source_.root, route, "/" + segment(route, source_.root));
        if (!diags_.hasErrors() && changed_) {
            result_.collectGarbage();
            result_.deduplicateShapes();
            result_.deduplicateLogVols();
            (void)result_.validate();
            destination = std::move(result_);
        }
        return std::move(diags_);
    }

  private:
    std::string segment(const OccurrenceId &route, const DaughterPlacement &edge) const {
        return std::string{tree_->pathNameFor(route, edge.name)};
    }
    const OccurrenceTags &tags(const OccurrenceId &route, LogVolId id) const {
        if (route.empty())
            return tree_->tagsFor(route, id);
        // The explicit overload avoids resolving every ancestor a second time.
        OccurrenceId parent(route.begin(), route.end() - 1);
        LogVolId parentId = source_.root.logVolId;
        for (const auto index : parent)
            parentId = source_.logVols.at(parentId).daughters[index].logVolId;
        return tree_->tagsFor(route, id, std::pair{parentId, route.back()});
    }
    Settings settings(const DaughterPlacement &edge, const OccurrenceId &route,
                      const std::string &path) const {
        const auto &volume = source_.logVols.at(edge.logVolId);
        const auto mat = source_.materials.find(volume.materialId);
        const auto &nodeTags = tags(route, edge.logVolId);
        selection::NodeView view{tree_->displayNameFor(route, edge.name),
                                 tree_->originalPathFor(route, path),
                                 mat == source_.materials.end() ? std::string_view{}
                                                                : mat->second.name,
                                 volume.daughters.empty(),
                                 &nodeTags,
                                 volume.name};
        Settings value;
        value.skip = config_.tessellationDefaults.skipGeometry.value_or(false);
        value.tess.maxSegmentsCircle = config_.tessellationDefaults.maxSegmentsCircle.value_or(64);
        value.tess.genericTrapTolerance =
            config_.tessellationDefaults.genericTrapTolerance.value_or(0.01);
        for (const auto &rule : rules_) {
            if (!rule.matches(view))
                continue;
            if (rule.source->geometry && !rule.source->geometry->replaceDescendants.empty()) {
                if (rule.source->geometry->replaceDescendants != "convex_hull")
                    throw Error{codes::kErrConfigParse, "unsupported geometry replacement"};
                value.replace = true;
            }
            if (rule.source->tessellation) {
                const auto &t = *rule.source->tessellation;
                if (t.skipGeometry)
                    value.skip = *t.skipGeometry;
                if (t.maxSegmentsCircle)
                    value.tess.maxSegmentsCircle = *t.maxSegmentsCircle;
                if (t.genericTrapTolerance)
                    value.tess.genericTrapTolerance = *t.genericTrapTolerance;
            }
        }
        // A generated hull is already a semantic solid, regardless of the old
        // assembly rule. Re-applying the same config is therefore idempotent.
        if (nodeTags.contains(kApproximationTag) &&
            nodeTags.at(kApproximationTag) == "convex_hull") {
            value.replace = false;
            value.skip = false;
        }
        return value;
    }

    const std::vector<glm::dvec3> &points(ShapeId id,
                                          const tessellation::TessellationParams &params) {
        const auto key =
            std::tuple{id.value, params.maxSegmentsCircle, params.genericTrapTolerance};
        if (auto it = points_.find(key); it != points_.end())
            return it->second;
        auto &out = points_[key];
        const auto &shape = source_.shapes.at(id).data;
        std::visit(
            [&](const auto &solid) {
                using T = std::decay_t<decltype(solid)>;
                if constexpr (std::is_same_v<T, BoxShape> || std::is_same_v<T, TrdShape>) {
                    for (int z : {-1, 1}) {
                        double dx, dy;
                        if constexpr (std::is_same_v<T, BoxShape>) {
                            dx = solid.dx;
                            dy = solid.dy;
                        } else {
                            dx = z < 0 ? solid.dx1 : solid.dx2;
                            dy = z < 0 ? solid.dy1 : solid.dy2;
                        }
                        for (int x : {-1, 1})
                            for (int y : {-1, 1})
                                out.emplace_back(x * dx, y * dy, z * solid.dz);
                    }
                } else if constexpr (std::is_same_v<T, GenericTrapShape>) {
                    for (std::size_t i = 0; i != 8; ++i)
                        out.emplace_back(solid.vertices[i], i < 4 ? -solid.dz : solid.dz);
                } else if constexpr (std::is_same_v<T, ExtrudedShape>) {
                    for (const auto &section : solid.sections)
                        for (const auto &vertex : solid.polygon)
                            out.emplace_back(vertex * section.scale + section.offset, section.z);
                } else if constexpr (std::is_same_v<T, TessellatedShape>) {
                    for (const auto &triangle : solid.triangles)
                        out.insert(out.end(), triangle.vertices.begin(), triangle.vertices.end());
                } else {
                    tessellation::PrimitiveTessellator primitive;
                    auto mesh = isBooleanShape(shape) ? tessellation::tessellateBooleanShape(
                                                            shape, source_, primitive, params)
                                                      : primitive.tessellate(shape, params);
                    diags_.append(mesh.diags);
                    if (!mesh.succeeded)
                        diags_.error(codes::kErrGeometryApproximation,
                                     "could not sample source shape for convex hull");
                    for (const auto &vertex : mesh.vertices)
                        out.emplace_back(vertex.position);
                }
            },
            shape);
        return out;
    }

    struct Contribution {
        ShapeId shape;
        glm::dmat4 transform;
        tessellation::TessellationParams params;
    };
    template <class T> static void append(std::string &key, const T &value) {
        key.append(reinterpret_cast<const char *>(&value), sizeof(value));
    }
    void collect(const DaughterPlacement &edge, OccurrenceId &route, const std::string &path,
                 const glm::dmat4 &transform, std::vector<Contribution> &parts, std::string &key) {
        const auto value = settings(edge, route, path);
        const auto &volume = source_.logVols.at(edge.logVolId);
        if (!value.skip && source_.shapes.contains(volume.shapeId)) {
            // Cache by the complete ordered contributing geometry and local
            // transforms, not a source volume ID: occurrence rules can differ.
            append(key, volume.shapeId.value);
            append(key, value.tess.maxSegmentsCircle);
            append(key, value.tess.genericTrapTolerance);
            for (int c = 0; c != 4; ++c)
                for (int r = 0; r != 4; ++r)
                    append(key, transform[c][r]);
            parts.push_back({volume.shapeId, transform, value.tess});
        }
        for (std::size_t i = 0; i != volume.daughters.size(); ++i) {
            const auto &child = volume.daughters[i];
            route.push_back(i);
            collect(child, route, path + "/" + segment(route, child),
                    transform * child.localTransform, parts, key);
            route.pop_back();
        }
    }
    ShapeId hull(const DaughterPlacement &edge, OccurrenceId &route, const std::string &path) {
        std::vector<Contribution> parts;
        std::string key;
        collect(edge, route, path, glm::dmat4{1.0}, parts, key);
        if (auto it = hulls_.find(key); it != hulls_.end())
            return it->second;
        std::vector<manifold::vec3> cloud;
        for (const auto &part : parts)
            for (const auto &point : points(part.shape, part.params)) {
                const auto p = part.transform * glm::dvec4{point, 1.0};
                cloud.emplace_back(p.x, p.y, p.z);
            }
        if (cloud.size() < 4) {
            diags_.error(codes::kErrGeometryApproximation,
                         "convex hull needs non-coplanar source geometry; subtree retained", path);
            return {};
        }
        const auto manifold = manifold::Manifold::Hull(cloud);
        if (manifold.Status() != manifold::Manifold::Error::NoError || manifold.IsEmpty() ||
            manifold.Volume() <= 0) {
            diags_.error(codes::kErrGeometryApproximation,
                         "convex hull is empty or degenerate; subtree retained", path);
            return {};
        }
        // Keep the semantic hull in double precision. Rendering can choose its
        // own precision later, and NHB must not bake a world-space float mesh.
        const auto mesh = manifold.GetMeshGL64();
        TessellatedShape solid;
        for (std::size_t i = 0; i < mesh.triVerts.size(); i += 3) {
            TessellatedShape::Triangle triangle;
            for (std::size_t j = 0; j != 3; ++j) {
                const auto vertex = static_cast<std::size_t>(mesh.triVerts[i + j] * mesh.numProp);
                triangle.vertices[j] = {mesh.vertProperties[vertex],
                                        mesh.vertProperties[vertex + 1],
                                        mesh.vertProperties[vertex + 2]};
            }
            solid.triangles.push_back(triangle);
        }
        const auto id = result_.nextShapeId();
        result_.shapes.emplace(id, Shape{id, std::move(solid)});
        hulls_.emplace(std::move(key), id);
        return id;
    }
    LogVolId clone(const LogicalVolume &source) {
        auto copy = source;
        copy.id = result_.nextLogVolId();
        const auto id = copy.id;
        result_.logVols.emplace(id, std::move(copy));
        if (auto it = source_.metadata.volumeTags.find(source.id);
            it != source_.metadata.volumeTags.end())
            result_.metadata.volumeTags[id] = it->second;
        for (std::size_t i = 0; i != source.daughters.size(); ++i)
            if (auto it = source_.metadata.placementTags.find({source.id, i});
                it != source_.metadata.placementTags.end())
                result_.metadata.placementTags[{id, i}] = it->second;
        return id;
    }
    LogVolId visit(const DaughterPlacement &edge, OccurrenceId &route, const std::string &path) {
        const auto &volume = source_.logVols.at(edge.logVolId);
        if (settings(edge, route, path).replace) {
            const auto shape = hull(edge, route, path);
            if (diags_.hasErrors())
                return edge.logVolId;
            changed_ = true;
            const auto id = clone(volume);
            auto &replacement = result_.logVols.at(id);
            replacement.shapeId = shape;
            replacement.daughters.clear();
            for (std::size_t i = 0; i != volume.daughters.size(); ++i)
                result_.metadata.placementTags.erase({id, i});
            auto nodeTags = tags(route, edge.logVolId);
            nodeTags[kApproximationTag] = "convex_hull";
            const auto tagId = result_.metadata.tagSets.size();
            result_.metadata.tagSets.push_back(std::move(nodeTags));
            result_.metadata.occurrenceTags[route] = tagId;
            eraseDescendants(result_.metadata.occurrenceTags, route);
            eraseDescendants(result_.metadata.displayNames, route);
            eraseDescendants(result_.metadata.pathNames, route);
            eraseDescendants(result_.metadata.originalPaths, route);
            eraseDescendants(result_.metadata.sourceSystems, route);
            eraseDescendants(result_.metadata.degradation, route);
            return id;
        }
        auto id = edge.logVolId;
        for (std::size_t i = 0; i != volume.daughters.size(); ++i) {
            const auto &child = volume.daughters[i];
            route.push_back(i);
            const auto replacement = visit(child, route, path + "/" + segment(route, child));
            route.pop_back();
            if (replacement != child.logVolId) {
                if (id == edge.logVolId)
                    id = clone(volume);
                result_.logVols.at(id).daughters[i].logVolId = replacement;
            }
        }
        return id;
    }
    const Scene &source_;
    const config::NHConfig &config_;
    std::unique_ptr<OccurrenceTree> tree_;
    Scene result_;
    DiagnosticList diags_;
    bool changed_{};
    std::vector<Rule> rules_;
    std::map<std::tuple<uint64_t, int, double>, std::vector<glm::dvec3>> points_;
    std::unordered_map<std::string, ShapeId> hulls_;
};
} // namespace

DiagnosticList approximate(ir::semantic::Scene &scene, const config::NHConfig &config) {
    if (std::none_of(config.rules.begin(), config.rules.end(),
                     [](const auto &rule) {
                         return rule.geometry && !rule.geometry->replaceDescendants.empty();
                     }) ||
        scene.logVols.empty())
        return {};
    return Approximation{scene, config}.run(scene);
}
} // namespace nodehammer::geometry

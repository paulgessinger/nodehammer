#include <ir/semantic.hpp>
#include <ir/semantic/occurrence_tree.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer::ir::semantic {
namespace {
void transform(const glm::dmat4 &matrix) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[col][row])) {
                throw std::invalid_argument("shared geometry contains a non-finite transform");
            }
        }
    }
    if (matrix[0][3] != 0 || matrix[1][3] != 0 || matrix[2][3] != 0 || matrix[3][3] != 1) {
        throw std::invalid_argument("shared geometry requires affine transforms");
    }
}

std::vector<ShapeId> operands(const Shape &shape) {
    return std::visit(
        [](const auto &data) -> std::vector<ShapeId> {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, BooleanUnion> ||
                          std::is_same_v<T, BooleanIntersection> ||
                          std::is_same_v<T, BooleanSubtraction>) {
                transform(data.rightTransform);
                return {data.left, data.right};
            }
            return {};
        },
        shape.data);
}

void shapeGraph(const GeometryCatalogs &scene) {
    ankerl::unordered_dense::set<ShapeId> done;
    ankerl::unordered_dense::set<ShapeId> active;
    done.reserve(scene.shapes.size());
    struct Frame {
        ShapeId id;
        std::vector<ShapeId> children;
        std::size_t next{};
    };
    for (const auto &[id, shape] : scene.shapes) {
        if (id != shape.id) {
            throw std::invalid_argument("shared shape key/id mismatch");
        }
        if (done.contains(id)) {
            continue;
        }
        std::vector<Frame> stack{{id, operands(shape)}};
        active.insert(id);
        while (!stack.empty()) {
            auto &frame = stack.back();
            if (frame.next == frame.children.size()) {
                done.insert(frame.id);
                active.erase(frame.id);
                stack.pop_back();
                continue;
            }
            const auto child = frame.children.at(frame.next++);
            auto it = scene.shapes.find(child);
            if (it == scene.shapes.end()) {
                throw std::invalid_argument("shared shape operand missing");
            }
            if (active.contains(child)) {
                throw std::invalid_argument("cycle in shared shape graph");
            }
            if (!done.contains(child)) {
                active.insert(child);
                stack.push_back({child, operands(it->second)});
            }
        }
    }
}

uint64_t volumeGraph(const GeometryCatalogs &scene, LogVolId root) {
    ankerl::unordered_dense::map<LogVolId, uint64_t> counts;
    ankerl::unordered_dense::set<LogVolId> active;
    counts.reserve(scene.logVols.size());
    struct Frame {
        LogVolId id;
        std::size_t next{};
        uint64_t count{1};
    };
    for (const auto &[id, volume] : scene.logVols) {
        if (id != volume.id) {
            throw std::invalid_argument("shared volume key/id mismatch");
        }
        if (counts.contains(id)) {
            continue;
        }
        std::vector<Frame> stack{{id}};
        active.insert(id);
        while (!stack.empty()) {
            auto &frame = stack.back();
            const auto &daughters = scene.logVols.at(frame.id).daughters;
            if (frame.next == daughters.size()) {
                counts.emplace(frame.id, frame.count);
                active.erase(frame.id);
                stack.pop_back();
                continue;
            }
            const auto child = daughters.at(frame.next).logVolId;
            auto it = counts.find(child);
            if (it == counts.end()) {
                if (!active.insert(child).second) {
                    throw std::invalid_argument("cycle in shared volume graph");
                }
                stack.push_back({child});
                continue;
            }
            if (it->second > std::numeric_limits<uint64_t>::max() - frame.count) {
                throw std::overflow_error("shared occurrence count exceeds uint64_t");
            }
            frame.count += it->second;
            ++frame.next;
        }
    }
    return counts.at(root);
}
} // namespace

uint64_t Scene::validate() const {
    if (!logVols.contains(root.logVolId)) {
        throw std::invalid_argument("shared geometry root volume missing");
    }
    transform(root.localTransform);
    for (const auto &[id, material] : materials) {
        if (id != material.id) {
            throw std::invalid_argument("shared material key/id mismatch");
        }
    }
    for (const auto &[id, volume] : logVols) {
        (void)id;
        if (!shapes.contains(volume.shapeId) || !materials.contains(volume.materialId)) {
            throw std::invalid_argument("shared volume shape/material missing");
        }
        for (const auto &daughter : volume.daughters) {
            if (!logVols.contains(daughter.logVolId)) {
                throw std::invalid_argument("shared daughter volume missing");
            }
            transform(daughter.localTransform);
        }
    }
    shapeGraph(*this);
    const auto count = volumeGraph(*this, root.logVolId);
    auto tag = [&](uint64_t index) {
        if (index >= metadata.tagSets.size()) {
            throw std::invalid_argument("shared metadata tag set missing");
        }
    };
    // Sparse metadata is sorted by route. Adjacent entries usually share
    // most ancestors, so retain the validated prefix rather than restarting
    // from the root for every entry. The longest-common-prefix check also
    // handles moving backwards when the next metadata map begins.
    const OccurrenceId *previous = nullptr;
    std::vector<LogVolId> routeVolumes{root.logVolId};
    auto occurrence = [&](const OccurrenceId &id) {
        std::size_t common = 0;
        if (previous != nullptr) {
            while (common < previous->size() && common < id.size() &&
                   previous->at(common) == id.at(common)) {
                ++common;
            }
        }
        routeVolumes.resize(common + 1);
        for (std::size_t depth = common; depth < id.size(); ++depth) {
            const auto &daughters = logVols.at(routeVolumes.back()).daughters;
            const auto slot = id.at(depth);
            if (slot >= daughters.size()) {
                throw std::invalid_argument("shared metadata occurrence missing");
            }
            routeVolumes.push_back(daughters.at(slot).logVolId);
        }
        previous = &id;
    };
    for (const auto &[volume, index] : metadata.volumeTags) {
        if (!logVols.contains(volume)) {
            throw std::invalid_argument("shared metadata volume missing");
        }
        tag(index);
    }
    for (const auto &[placement, index] : metadata.placementTags) {
        const auto it = logVols.find(placement.first);
        if (it == logVols.end() || placement.second >= it->second.daughters.size()) {
            throw std::invalid_argument("shared metadata daughter missing");
        }
        tag(index);
    }
    for (const auto &[id, index] : metadata.occurrenceTags) {
        occurrence(id);
        tag(index);
    }
    for (const auto &[id, name] : metadata.displayNames) {
        (void)name;
        occurrence(id);
    }
    for (const auto &[id, name] : metadata.pathNames) {
        (void)name;
        occurrence(id);
    }
    for (const auto &[id, system] : metadata.sourceSystems) {
        (void)system;
        occurrence(id);
    }
    for (const auto &[id, path] : metadata.originalPaths) {
        (void)path;
        occurrence(id);
    }
    for (const auto &[id, flags] : metadata.degradation) {
        (void)flags;
        occurrence(id);
    }
    return count;
}

std::unique_ptr<semantic::OccurrenceTree> Scene::makeTree(std::size_t capacity) const {
    (void)validate();
    auto tree = std::make_unique<semantic::OccurrenceTree>(*this, root, capacity);
    // validate() has checked every route and tag ID. Replaying the public
    // setters would traverse every route again and insert sorted maps in
    // O(n log n), despite the fresh tree having no snapshots to invalidate.
    tree->defaultSourceSystem_ = metadata.defaultSourceSystem;
    tree->displayNames_ = metadata.displayNames;
    tree->pathNames_ = metadata.pathNames;
    tree->sourceSystems_ = metadata.sourceSystems;
    tree->originalPaths_ = metadata.originalPaths;
    tree->degradation_ = metadata.degradation;

    // Metadata IDs need not equal interned IDs (duplicate and unused source
    // tag sets are allowed). Intern each referenced definition only once.
    std::vector<std::optional<std::size_t>> tagIds(metadata.tagSets.size());
    const auto internTag = [&](uint64_t id) {
        const auto index = static_cast<std::size_t>(id);
        auto &mapped = tagIds.at(index);
        if (!mapped) {
            mapped = tree->intern(metadata.tagSets.at(index));
        }
        return *mapped;
    };
    for (const auto &[volume, tag] : metadata.volumeTags) {
        tree->volumeTags_.emplace_hint(tree->volumeTags_.end(), volume, internTag(tag));
    }
    for (const auto &[slot, tag] : metadata.placementTags) {
        tree->placementTags_.emplace_hint(tree->placementTags_.end(), slot, internTag(tag));
    }
    for (const auto &[id, tag] : metadata.occurrenceTags) {
        tree->tags_.emplace_hint(tree->tags_.end(), id, internTag(tag));
    }
    return tree;
}

std::size_t Scene::deduplicateLogVols() {
    // Source-level tag keys would need remapping before definitions can merge.
    if (!metadata.volumeTags.empty() || !metadata.placementTags.empty())
        return 0;
    const auto remap = deduplicateVolumeDefinitions();
    if (auto it = remap.find(root.logVolId); it != remap.end())
        root.logVolId = it->second;
    return remap.size();
}
void Scene::collectGarbage() {
    std::unordered_set<LogVolId> volumes;
    std::vector<LogVolId> pending{root.logVolId};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!volumes.insert(id).second)
            continue;
        for (const auto &daughter : logVols.at(id).daughters)
            pending.push_back(daughter.logVolId);
    }
    std::erase_if(logVols, [&](const auto &row) { return !volumes.contains(row.first); });
    std::unordered_set<ShapeId> referencedShapes;
    std::unordered_set<MaterialId> referencedMaterials;
    std::vector<ShapeId> shapesPending;
    for (const auto &[id, volume] : logVols) {
        (void)id;
        shapesPending.push_back(volume.shapeId);
        referencedMaterials.insert(volume.materialId);
    }
    while (!shapesPending.empty()) {
        const auto id = shapesPending.back();
        shapesPending.pop_back();
        if (!referencedShapes.insert(id).second)
            continue;
        std::visit(
            [&](const auto &shape) {
                if constexpr (is_boolean_shape_v<std::decay_t<decltype(shape)>>) {
                    shapesPending.push_back(shape.left);
                    shapesPending.push_back(shape.right);
                }
            },
            shapes.at(id).data);
    }
    std::erase_if(shapes, [&](const auto &row) { return !referencedShapes.contains(row.first); });
    std::erase_if(materials,
                  [&](const auto &row) { return !referencedMaterials.contains(row.first); });
    std::erase_if(metadata.volumeTags,
                  [&](const auto &row) { return !volumes.contains(row.first); });
    std::erase_if(metadata.placementTags,
                  [&](const auto &row) { return !volumes.contains(row.first.first); });
}
} // namespace nodehammer::ir::semantic

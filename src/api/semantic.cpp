#include <nodehammer/scene.hpp>

#include <nodehammer/detail/scene_access.hpp>

#include "state.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nodehammer {

namespace {

/// ShapeKind mirrors SemanticShapeVariant's alternative order, so the mapping is
/// the variant index. These assertions are what make that safe: adding or
/// reordering an alternative breaks the build here instead of silently
/// reporting the wrong kind.
static_assert(std::variant_size_v<SemanticShapeVariant> == 13,
              "SemanticShapeVariant gained an alternative — extend ShapeKind to match");
static_assert(std::is_same_v<std::variant_alternative_t<0, SemanticShapeVariant>, BoxShape>);
static_assert(
    std::is_same_v<std::variant_alternative_t<8, SemanticShapeVariant>, TessellatedShape>);
static_assert(std::is_same_v<std::variant_alternative_t<9, SemanticShapeVariant>, BooleanUnion>);
static_assert(
    std::is_same_v<std::variant_alternative_t<11, SemanticShapeVariant>, BooleanSubtraction>);
static_assert(std::is_same_v<std::variant_alternative_t<12, SemanticShapeVariant>, UnknownShape>);

template <typename Id, typename Map> std::vector<Id> sortedIds(const Map &map) {
    std::vector<Id> ids;
    ids.reserve(map.size());
    for (const auto &[id, _] : map) {
        ids.push_back(id);
    }
    std::ranges::sort(ids, [](Id a, Id b) { return a.value < b.value; });
    return ids;
}

} // namespace

namespace detail {

SemanticSceneState::SemanticSceneState(std::shared_ptr<const SemanticScene> s)
    : scene(std::move(s)) {
    if (!scene) {
        return;
    }

    logVolIds = sortedIds<SemanticLogVolId>(scene->logVols);
    shapeIds = sortedIds<SemanticShapeId>(scene->shapes);
    materialIds = sortedIds<SemanticMaterialId>(scene->materials);

    stats.nodeCount = scene->nodes.size();
    stats.logicalVolumeCount = scene->logVols.size();
    stats.shapeCount = scene->shapes.size();
    stats.materialCount = scene->materials.size();

    for (const auto &[id, shape] : scene->shapes) {
        if (isBooleanShape(shape.data)) {
            ++stats.booleanShapeCount;
        }
        ++shapeKindCounts[std::string{shapeKindName(static_cast<ShapeKind>(shape.data.index()))}];
    }

    // Depth-first preorder, guarding both a missing id and a repeat visit so a
    // cyclic or partially-pruned graph terminates rather than trapping.
    nodeIds.reserve(scene->nodes.size());
    std::unordered_set<SemanticNodeId> seen;
    seen.reserve(scene->nodes.size());

    struct Frame {
        SemanticNodeId id;
        int depth;
    };
    std::vector<Frame> stack;
    if (scene->nodes.contains(scene->rootId)) {
        stack.push_back({scene->rootId, 0});
    }

    while (!stack.empty()) {
        const auto frame = stack.back();
        stack.pop_back();

        const auto it = scene->nodes.find(frame.id);
        if (it == scene->nodes.end() || !seen.insert(frame.id).second) {
            continue;
        }
        const auto &node = it->second;

        nodeIds.push_back(frame.id);
        stats.maxDepth = std::max(stats.maxDepth, frame.depth);
        if (node.children.empty()) {
            ++stats.leafCount;
        }

        for (std::size_t i = node.children.size(); i-- > 0;) {
            stack.push_back({node.children[i], frame.depth + 1});
        }
    }
    stats.reachableNodeCount = nodeIds.size();
}

} // namespace detail

// ── SemanticScene ─────────────────────────────────────────────────────────────

SemanticScene::SemanticScene() noexcept = default;
SemanticScene::SemanticScene(const SemanticScene &) = default;
SemanticScene::SemanticScene(SemanticScene &&) noexcept = default;
SemanticScene &SemanticScene::operator=(const SemanticScene &) = default;
SemanticScene &SemanticScene::operator=(SemanticScene &&) noexcept = default;
SemanticScene::~SemanticScene() = default;

SemanticScene::SemanticScene(std::shared_ptr<const detail::SemanticSceneState> state) noexcept
    : state_(std::move(state)) {}

bool SemanticScene::valid() const noexcept { return state_ != nullptr && state_->scene != nullptr; }

std::string_view SemanticScene::sourceFile() const noexcept {
    return valid() ? std::string_view{state_->scene->sourceFile} : std::string_view{};
}

std::size_t SemanticScene::nodeCount() const noexcept {
    return valid() ? state_->stats.nodeCount : 0;
}

std::size_t SemanticScene::logicalVolumeCount() const noexcept {
    return valid() ? state_->stats.logicalVolumeCount : 0;
}

std::size_t SemanticScene::shapeCount() const noexcept {
    return valid() ? state_->stats.shapeCount : 0;
}

std::size_t SemanticScene::materialCount() const noexcept {
    return valid() ? state_->stats.materialCount : 0;
}

const SemanticStats &SemanticScene::stats() const noexcept {
    static const SemanticStats empty;
    return valid() ? state_->stats : empty;
}

std::vector<ShapeKindCount> SemanticScene::shapeKindCounts() const {
    if (!valid()) {
        return {};
    }
    std::vector<ShapeKindCount> counts;
    counts.reserve(state_->shapeKindCounts.size());
    for (const auto &[kind, count] : state_->shapeKindCounts) {
        counts.push_back(ShapeKindCount{kind, count});
    }
    return counts;
}

std::vector<std::string> SemanticScene::materialNames() const {
    if (!valid()) {
        return {};
    }
    std::vector<std::string> names;
    names.reserve(state_->materialIds.size());
    for (const auto id : state_->materialIds) {
        names.emplace_back(state_->scene->materials.at(id).name);
    }
    return names;
}
// ── Seam ──────────────────────────────────────────────────────────────────────

SemanticScene wrapSemanticScene(std::shared_ptr<const detail::SemanticScene> scene) {
    if (!scene) {
        return SemanticScene{};
    }
    return SemanticScene{std::make_shared<const detail::SemanticSceneState>(std::move(scene))};
}

SemanticScene wrapSemanticScene(detail::SemanticScene scene) {
    return wrapSemanticScene(std::make_shared<const detail::SemanticScene>(std::move(scene)));
}

const std::shared_ptr<const detail::SemanticScene> &
unwrapSemanticScene(const SemanticScene &handle) noexcept {
    static const std::shared_ptr<const detail::SemanticScene> empty;
    return handle.state_ ? handle.state_->scene : empty;
}

const std::shared_ptr<const detail::SemanticSceneState> &
unwrapSemanticSceneState(const SemanticScene &handle) noexcept {
    static const std::shared_ptr<const detail::SemanticSceneState> empty;
    return handle.state_ ? handle.state_ : empty;
}

} // namespace nodehammer

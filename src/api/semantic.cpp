#include <nodehammer/scene.hpp>

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

std::string_view shapeKindName(ShapeKind kind) noexcept {
    switch (kind) {
    case ShapeKind::Box:
        return "box";
    case ShapeKind::Tube:
        return "tube";
    case ShapeKind::Cone:
        return "cone";
    case ShapeKind::Trd:
        return "trd";
    case ShapeKind::Para:
        return "para";
    case ShapeKind::Pcon:
        return "pcon";
    case ShapeKind::Pgon:
        return "pgon";
    case ShapeKind::Torus:
        return "torus";
    case ShapeKind::Tessellated:
        return "tessellated";
    case ShapeKind::Union:
        return "union";
    case ShapeKind::Intersection:
        return "intersection";
    case ShapeKind::Subtraction:
        return "subtraction";
    case ShapeKind::Unknown:
        break;
    }
    return "unknown";
}

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

// ── SourceMaterialView ────────────────────────────────────────────────────────

SourceMaterialView::SourceMaterialView(std::shared_ptr<const detail::SemanticSceneState> state,
                                       const SourceMaterial *material) noexcept
    : state_(std::move(state)), material_(material) {}

SemanticMaterialId SourceMaterialView::id() const noexcept { return material_->id; }
std::string_view SourceMaterialView::name() const noexcept { return material_->name; }
double SourceMaterialView::density() const noexcept { return material_->density; }
std::optional<glm::vec3> SourceMaterialView::color() const noexcept { return material_->color; }

// ── ShapeView ─────────────────────────────────────────────────────────────────

ShapeView::ShapeView(std::shared_ptr<const detail::SemanticSceneState> state,
                     const SemanticShape *shape) noexcept
    : state_(std::move(state)), shape_(shape) {}

SemanticShapeId ShapeView::id() const noexcept { return shape_->id; }

ShapeKind ShapeView::kind() const noexcept { return static_cast<ShapeKind>(shape_->data.index()); }

std::string_view ShapeView::kindName() const noexcept { return shapeKindName(kind()); }

bool ShapeView::isBoolean() const noexcept { return isBooleanShape(shape_->data); }

std::string_view ShapeView::originalType() const noexcept {
    if (const auto *unknown = std::get_if<UnknownShape>(&shape_->data)) {
        return unknown->originalType;
    }
    return {};
}

std::optional<SemanticShapeId> ShapeView::booleanLeft() const noexcept {
    return std::visit(
        [](const auto &s) -> std::optional<SemanticShapeId> {
            if constexpr (is_boolean_shape_v<std::decay_t<decltype(s)>>) {
                return s.left;
            } else {
                return std::nullopt;
            }
        },
        shape_->data);
}

std::optional<SemanticShapeId> ShapeView::booleanRight() const noexcept {
    return std::visit(
        [](const auto &s) -> std::optional<SemanticShapeId> {
            if constexpr (is_boolean_shape_v<std::decay_t<decltype(s)>>) {
                return s.right;
            } else {
                return std::nullopt;
            }
        },
        shape_->data);
}

std::size_t ShapeView::triangleCount() const noexcept {
    if (const auto *tess = std::get_if<TessellatedShape>(&shape_->data)) {
        return tess->triangles.size();
    }
    return 0;
}

// ── LogicalVolumeView ─────────────────────────────────────────────────────────

LogicalVolumeView::LogicalVolumeView(std::shared_ptr<const detail::SemanticSceneState> state,
                                     const SemanticLogicalVolume *logVol) noexcept
    : state_(std::move(state)), logVol_(logVol) {}

SemanticLogVolId LogicalVolumeView::id() const noexcept { return logVol_->id; }
std::string_view LogicalVolumeView::name() const noexcept { return logVol_->name; }
SemanticShapeId LogicalVolumeView::shapeId() const noexcept { return logVol_->shapeId; }
SemanticMaterialId LogicalVolumeView::materialId() const noexcept { return logVol_->materialId; }
std::size_t LogicalVolumeView::daughterCount() const noexcept { return logVol_->daughters.size(); }

std::optional<ShapeView> LogicalVolumeView::shape() const {
    const auto &shapes = state_->scene->shapes;
    const auto it = shapes.find(logVol_->shapeId);
    return it == shapes.end() ? std::nullopt : std::optional{ShapeView{state_, &it->second}};
}

std::optional<SourceMaterialView> LogicalVolumeView::material() const {
    const auto &materials = state_->scene->materials;
    const auto it = materials.find(logVol_->materialId);
    return it == materials.end() ? std::nullopt
                                 : std::optional{SourceMaterialView{state_, &it->second}};
}

// ── SemanticNodeView ──────────────────────────────────────────────────────────

SemanticNodeView::SemanticNodeView(std::shared_ptr<const detail::SemanticSceneState> state,
                                   const SemanticNode *node) noexcept
    : state_(std::move(state)), node_(node) {}

SemanticNodeId SemanticNodeView::id() const noexcept { return node_->id; }
std::string_view SemanticNodeView::name() const noexcept { return node_->name; }
std::string_view SemanticNodeView::originalPath() const noexcept { return node_->originalPath; }
std::string_view SemanticNodeView::sourceSystem() const noexcept { return node_->sourceSystem; }

std::optional<SemanticNodeId> SemanticNodeView::parentId() const noexcept {
    return node_->parentId;
}

std::span<const SemanticNodeId> SemanticNodeView::childIds() const noexcept {
    return node_->children;
}

std::size_t SemanticNodeView::childCount() const noexcept { return node_->children.size(); }
bool SemanticNodeView::isLeaf() const noexcept { return node_->children.empty(); }
SemanticLogVolId SemanticNodeView::logVolId() const noexcept { return node_->logVolId; }

const glm::dmat4 &SemanticNodeView::localTransform() const noexcept {
    return node_->localTransform;
}

const glm::dmat4 &SemanticNodeView::worldTransform() const noexcept {
    return node_->worldTransform;
}

std::size_t SemanticNodeView::tagCount() const noexcept { return node_->tags.size(); }

std::optional<std::string_view> SemanticNodeView::tag(std::string_view key) const {
    const auto it = node_->tags.find(std::string{key});
    return it == node_->tags.end() ? std::nullopt : std::optional<std::string_view>{it->second};
}

void SemanticNodeView::forEachTag(
    const std::function<void(std::string_view, std::string_view)> &fn) const {
    for (const auto &[key, value] : node_->tags) {
        fn(key, value);
    }
}

std::optional<LogicalVolumeView> SemanticNodeView::logicalVolume() const {
    const auto &logVols = state_->scene->logVols;
    const auto it = logVols.find(node_->logVolId);
    return it == logVols.end() ? std::nullopt
                               : std::optional{LogicalVolumeView{state_, &it->second}};
}

std::optional<ShapeView> SemanticNodeView::shape() const {
    const auto lv = logicalVolume();
    return lv ? lv->shape() : std::nullopt;
}

std::optional<SourceMaterialView> SemanticNodeView::material() const {
    const auto lv = logicalVolume();
    return lv ? lv->material() : std::nullopt;
}

SemanticScene SemanticNodeView::scene() const { return SemanticScene{state_}; }

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

SemanticNodeId SemanticScene::rootId() const noexcept {
    return valid() ? state_->scene->rootId : SemanticNodeId{};
}

std::optional<SemanticNodeView> SemanticScene::root() const {
    return valid() ? node(state_->scene->rootId) : std::nullopt;
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

std::optional<SemanticNodeView> SemanticScene::node(SemanticNodeId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto &nodes = state_->scene->nodes;
    const auto it = nodes.find(id);
    return it == nodes.end() ? std::nullopt : std::optional{SemanticNodeView{state_, &it->second}};
}

std::optional<LogicalVolumeView> SemanticScene::logicalVolume(SemanticLogVolId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto &logVols = state_->scene->logVols;
    const auto it = logVols.find(id);
    return it == logVols.end() ? std::nullopt
                               : std::optional{LogicalVolumeView{state_, &it->second}};
}

std::optional<ShapeView> SemanticScene::shape(SemanticShapeId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto &shapes = state_->scene->shapes;
    const auto it = shapes.find(id);
    return it == shapes.end() ? std::nullopt : std::optional{ShapeView{state_, &it->second}};
}

std::optional<SourceMaterialView> SemanticScene::material(SemanticMaterialId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto &materials = state_->scene->materials;
    const auto it = materials.find(id);
    return it == materials.end() ? std::nullopt
                                 : std::optional{SourceMaterialView{state_, &it->second}};
}

std::span<const SemanticNodeId> SemanticScene::nodeIds() const noexcept {
    return state_ ? std::span<const SemanticNodeId>{state_->nodeIds}
                  : std::span<const SemanticNodeId>{};
}

std::span<const SemanticLogVolId> SemanticScene::logicalVolumeIds() const noexcept {
    return state_ ? std::span<const SemanticLogVolId>{state_->logVolIds}
                  : std::span<const SemanticLogVolId>{};
}

std::span<const SemanticShapeId> SemanticScene::shapeIds() const noexcept {
    return state_ ? std::span<const SemanticShapeId>{state_->shapeIds}
                  : std::span<const SemanticShapeId>{};
}

std::span<const SemanticMaterialId> SemanticScene::materialIds() const noexcept {
    return state_ ? std::span<const SemanticMaterialId>{state_->materialIds}
                  : std::span<const SemanticMaterialId>{};
}

void SemanticScene::traverse(const Visitor &fn) const {
    if (valid()) {
        traverseFrom(state_->scene->rootId, fn);
    }
}

void SemanticScene::traverseFrom(SemanticNodeId start, const Visitor &fn) const {
    if (!valid()) {
        return;
    }
    const auto &nodes = state_->scene->nodes;
    if (!nodes.contains(start)) {
        return;
    }

    struct Frame {
        SemanticNodeId id;
        int depth;
        std::size_t siblingIndex;
        std::size_t siblingCount;
    };

    std::vector<Frame> stack{{start, 0, 0, 1}};
    std::unordered_set<SemanticNodeId> seen;
    seen.reserve(nodes.size());

    while (!stack.empty()) {
        const auto frame = stack.back();
        stack.pop_back();

        const auto it = nodes.find(frame.id);
        if (it == nodes.end() || !seen.insert(frame.id).second) {
            continue;
        }
        const auto &node = it->second;

        const Visit visit{SemanticNodeView{state_, &node}, frame.depth, frame.siblingIndex,
                          frame.siblingCount, frame.siblingIndex + 1 == frame.siblingCount};
        if (!fn(visit)) {
            continue; // caller pruned this subtree
        }

        const auto count = node.children.size();
        // Reverse push so children pop in stored order.
        for (std::size_t i = count; i-- > 0;) {
            stack.push_back({node.children[i], frame.depth + 1, i, count});
        }
    }
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

} // namespace nodehammer

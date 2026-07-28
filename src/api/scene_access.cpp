#include <nodehammer/detail/handle_seam.hpp>

#include <algorithm>
#include <nodehammer/detail/scene_access.hpp>

#include <unordered_set>
#include <utility>
#include <vector>

namespace nodehammer::detail {

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

// ── SourceMaterialView ────────────────────────────────────────────────────────

SourceMaterialView::SourceMaterialView(std::shared_ptr<const SemanticScene> scene,
                                       const SourceMaterial *material) noexcept
    : scene_(std::move(scene)), material_(material) {}

SemanticMaterialId SourceMaterialView::id() const noexcept { return material_->id; }
std::string_view SourceMaterialView::name() const noexcept { return material_->name; }
double SourceMaterialView::density() const noexcept { return material_->density; }
std::optional<glm::vec3> SourceMaterialView::color() const noexcept { return material_->color; }

// ── ShapeView ─────────────────────────────────────────────────────────────────

ShapeView::ShapeView(std::shared_ptr<const SemanticScene> scene,
                     const SemanticShape *shape) noexcept
    : scene_(std::move(scene)), shape_(shape) {}

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

LogicalVolumeView::LogicalVolumeView(std::shared_ptr<const SemanticScene> scene,
                                     const SemanticLogicalVolume *logVol) noexcept
    : scene_(std::move(scene)), logVol_(logVol) {}

SemanticLogVolId LogicalVolumeView::id() const noexcept { return logVol_->id; }
std::string_view LogicalVolumeView::name() const noexcept { return logVol_->name; }
SemanticShapeId LogicalVolumeView::shapeId() const noexcept { return logVol_->shapeId; }
SemanticMaterialId LogicalVolumeView::materialId() const noexcept { return logVol_->materialId; }
std::size_t LogicalVolumeView::daughterCount() const noexcept { return logVol_->daughters.size(); }

std::optional<ShapeView> LogicalVolumeView::shape() const {
    const auto &shapes = scene_->shapes;
    const auto it = shapes.find(logVol_->shapeId);
    return it == shapes.end() ? std::nullopt : std::optional{ShapeView{scene_, &it->second}};
}

std::optional<SourceMaterialView> LogicalVolumeView::material() const {
    const auto &materials = scene_->materials;
    const auto it = materials.find(logVol_->materialId);
    return it == materials.end() ? std::nullopt
                                 : std::optional{SourceMaterialView{scene_, &it->second}};
}

// ── SemanticNodeView ──────────────────────────────────────────────────────────

SemanticNodeView::SemanticNodeView(std::shared_ptr<const SemanticScene> scene,
                                   const SemanticNode *node) noexcept
    : scene_(std::move(scene)), node_(node) {}

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
    const auto &logVols = scene_->logVols;
    const auto it = logVols.find(node_->logVolId);
    return it == logVols.end() ? std::nullopt
                               : std::optional{LogicalVolumeView{scene_, &it->second}};
}

std::optional<ShapeView> SemanticNodeView::shape() const {
    const auto lv = logicalVolume();
    return lv ? lv->shape() : std::nullopt;
}

std::optional<SourceMaterialView> SemanticNodeView::material() const {
    const auto lv = logicalVolume();
    return lv ? lv->material() : std::nullopt;
}

// ── Lookup ────────────────────────────────────────────────────────────────────

namespace {

template <typename View, typename Map, typename Id>
std::optional<View> lookup(const nodehammer::SemanticScene &handle, const Map &map, Id id) {
    const auto it = map.find(id);
    return it == map.end() ? std::nullopt
                           : std::optional<View>{View{unwrapSemanticScene(handle), &it->second}};
}

} // namespace

std::optional<SemanticNodeView> node(const nodehammer::SemanticScene &handle, SemanticNodeId id) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? lookup<SemanticNodeView>(handle, scene->nodes, id) : std::nullopt;
}

std::optional<SemanticNodeView> root(const nodehammer::SemanticScene &handle) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? node(handle, scene->rootId) : std::nullopt;
}

std::optional<LogicalVolumeView> logicalVolume(const nodehammer::SemanticScene &handle,
                                               SemanticLogVolId id) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? lookup<LogicalVolumeView>(handle, scene->logVols, id) : std::nullopt;
}

std::optional<ShapeView> shape(const nodehammer::SemanticScene &handle, SemanticShapeId id) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? lookup<ShapeView>(handle, scene->shapes, id) : std::nullopt;
}

std::optional<SourceMaterialView> material(const nodehammer::SemanticScene &handle,
                                           SemanticMaterialId id) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? lookup<SourceMaterialView>(handle, scene->materials, id) : std::nullopt;
}

namespace {

template <typename Id, typename Map> std::vector<Id> sortedIds(const Map &map) {
    std::vector<Id> ids;
    ids.reserve(map.size());
    for (const auto &[id, value] : map) {
        ids.push_back(id);
    }
    std::ranges::sort(ids, [](Id a, Id b) { return a.value < b.value; });
    return ids;
}

} // namespace

std::vector<SemanticNodeId> nodeIds(const nodehammer::SemanticScene &handle) {
    // Depth-first preorder from the root, so the order is a property of the tree
    // rather than of the backing container. Guards a missing id and a repeat
    // visit, so a cyclic or partially-pruned graph terminates.
    const auto &scene = unwrapSemanticScene(handle);
    if (!scene) {
        return {};
    }

    std::vector<SemanticNodeId> ids;
    ids.reserve(scene->nodes.size());
    std::unordered_set<SemanticNodeId> seen;
    seen.reserve(scene->nodes.size());

    std::vector<SemanticNodeId> stack;
    if (scene->nodes.contains(scene->rootId)) {
        stack.push_back(scene->rootId);
    }
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        const auto it = scene->nodes.find(id);
        if (it == scene->nodes.end() || !seen.insert(id).second) {
            continue;
        }
        ids.push_back(id);
        const auto &children = it->second.children;
        for (std::size_t i = children.size(); i-- > 0;) {
            stack.push_back(children[i]);
        }
    }
    return ids;
}

std::vector<SemanticLogVolId> logicalVolumeIds(const nodehammer::SemanticScene &handle) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? sortedIds<SemanticLogVolId>(scene->logVols) : std::vector<SemanticLogVolId>{};
}

std::vector<SemanticShapeId> shapeIds(const nodehammer::SemanticScene &handle) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? sortedIds<SemanticShapeId>(scene->shapes) : std::vector<SemanticShapeId>{};
}

std::vector<SemanticMaterialId> materialIds(const nodehammer::SemanticScene &handle) {
    const auto &scene = unwrapSemanticScene(handle);
    return scene ? sortedIds<SemanticMaterialId>(scene->materials)
                 : std::vector<SemanticMaterialId>{};
}

// ── Traversal ─────────────────────────────────────────────────────────────────

void traverse(const nodehammer::SemanticScene &handle, const Visitor &fn) {
    const auto &scene = unwrapSemanticScene(handle);
    if (scene) {
        traverseFrom(handle, scene->rootId, fn);
    }
}

void traverseFrom(const nodehammer::SemanticScene &handle, SemanticNodeId start,
                  const Visitor &fn) {
    const auto &scenePtr = unwrapSemanticScene(handle);
    if (!scenePtr) {
        return;
    }
    const auto &nodes = scenePtr->nodes;
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
        const auto &n = it->second;

        const Visit visit{SemanticNodeView{scenePtr, &n}, frame.depth, frame.siblingIndex,
                          frame.siblingCount, frame.siblingIndex + 1 == frame.siblingCount};
        if (!fn(visit)) {
            continue; // caller pruned this subtree
        }

        const auto count = n.children.size();
        // Reverse push so children pop in stored order.
        for (std::size_t i = count; i-- > 0;) {
            stack.push_back({n.children[i], frame.depth + 1, i, count});
        }
    }
}

} // namespace nodehammer::detail

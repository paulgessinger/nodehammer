#pragma once

// Public handles over the semantic IR. Same contract as <nodehammer/render.hpp>:
// value outside, shared_ptr inside, read-only, no field access.
//
// Views (nodes, logical volumes, shapes, materials) each carry a refcount on
// the scene, so any of them may outlive the SemanticScene handle it came from
// and the string_views they hand out stay valid for as long as the view does.
//
// This header is on the amalgamated header's C++20 floor and must stay clear of
// nlohmann. glm and unordered_dense are fine — both are shimmed there.

#include <nodehammer/ir/semantic.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace nodehammer {

namespace detail {
class SemanticScene;
struct SemanticSceneState;
} // namespace detail

class SemanticScene;
class SemanticNodeView;
class LogicalVolumeView;
class ShapeView;
class SourceMaterialView;

// ── Shape kind ────────────────────────────────────────────────────────────────

/// The shape variants, collapsed to a flat enum. Consumers that only want to
/// say "what kind of solid is this" should not have to visit a 13-way variant —
/// and keeping the parameters out of the surface is what lets the variant gain
/// alternatives without breaking anyone.
enum class ShapeKind : std::uint8_t {
    Box,
    Tube,
    Cone,
    Trd,
    Para,
    Pcon,
    Pgon,
    Torus,
    Tessellated,
    Union,
    Intersection,
    Subtraction,
    Unknown,
};

/// Lowercase, stable across releases: "box", "tube", ..., "union", "unknown".
[[nodiscard]] std::string_view shapeKindName(ShapeKind kind) noexcept;

[[nodiscard]] constexpr bool isBoolean(ShapeKind kind) noexcept {
    return kind == ShapeKind::Union || kind == ShapeKind::Intersection ||
           kind == ShapeKind::Subtraction;
}

// ── Views ─────────────────────────────────────────────────────────────────────

class SourceMaterialView {
  public:
    [[nodiscard]] SemanticMaterialId id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] double density() const noexcept;
    [[nodiscard]] std::optional<glm::vec3> color() const noexcept;

  private:
    friend class SemanticScene;
    friend class SemanticNodeView;
    friend class LogicalVolumeView;
    SourceMaterialView(std::shared_ptr<const detail::SemanticSceneState> state,
                       const SourceMaterial *material) noexcept;

    std::shared_ptr<const detail::SemanticSceneState> state_;
    const SourceMaterial *material_{nullptr};
};

class ShapeView {
  public:
    [[nodiscard]] SemanticShapeId id() const noexcept;
    [[nodiscard]] ShapeKind kind() const noexcept;
    [[nodiscard]] std::string_view kindName() const noexcept;
    [[nodiscard]] bool isBoolean() const noexcept;

    /// The source system's own type name. Non-empty only for ShapeKind::Unknown,
    /// where it is the only record of what could not be translated.
    [[nodiscard]] std::string_view originalType() const noexcept;

    /// Boolean operands; nullopt for non-boolean shapes.
    [[nodiscard]] std::optional<SemanticShapeId> booleanLeft() const noexcept;
    [[nodiscard]] std::optional<SemanticShapeId> booleanRight() const noexcept;

    /// Triangle count for a tessellated shape; 0 for every other kind.
    [[nodiscard]] std::size_t triangleCount() const noexcept;

  private:
    friend class SemanticScene;
    friend class SemanticNodeView;
    friend class LogicalVolumeView;
    ShapeView(std::shared_ptr<const detail::SemanticSceneState> state,
              const SemanticShape *shape) noexcept;

    std::shared_ptr<const detail::SemanticSceneState> state_;
    const SemanticShape *shape_{nullptr};
};

class LogicalVolumeView {
  public:
    [[nodiscard]] SemanticLogVolId id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] SemanticShapeId shapeId() const noexcept;
    [[nodiscard]] SemanticMaterialId materialId() const noexcept;
    [[nodiscard]] std::size_t daughterCount() const noexcept;

    [[nodiscard]] std::optional<ShapeView> shape() const;
    [[nodiscard]] std::optional<SourceMaterialView> material() const;

  private:
    friend class SemanticScene;
    friend class SemanticNodeView;
    LogicalVolumeView(std::shared_ptr<const detail::SemanticSceneState> state,
                      const SemanticLogicalVolume *logVol) noexcept;

    std::shared_ptr<const detail::SemanticSceneState> state_;
    const SemanticLogicalVolume *logVol_{nullptr};
};

class SemanticNodeView {
  public:
    [[nodiscard]] SemanticNodeId id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    /// Full path from the root in the source tree, e.g. "/world/ODD/sensor_0".
    [[nodiscard]] std::string_view originalPath() const noexcept;
    [[nodiscard]] std::string_view sourceSystem() const noexcept;

    [[nodiscard]] std::optional<SemanticNodeId> parentId() const noexcept;
    [[nodiscard]] std::span<const SemanticNodeId> childIds() const noexcept;
    [[nodiscard]] std::size_t childCount() const noexcept;
    [[nodiscard]] bool isLeaf() const noexcept;

    [[nodiscard]] SemanticLogVolId logVolId() const noexcept;
    [[nodiscard]] const glm::dmat4 &localTransform() const noexcept;
    [[nodiscard]] const glm::dmat4 &worldTransform() const noexcept;

    [[nodiscard]] std::size_t tagCount() const noexcept;
    [[nodiscard]] std::optional<std::string_view> tag(std::string_view key) const;
    /// Tags in key order.
    void forEachTag(const std::function<void(std::string_view, std::string_view)> &fn) const;

    /// The node -> logical volume -> {shape, material} chain. Nullopt when a
    /// link is missing, which a partially-degraded import can produce.
    [[nodiscard]] std::optional<LogicalVolumeView> logicalVolume() const;
    [[nodiscard]] std::optional<ShapeView> shape() const;
    [[nodiscard]] std::optional<SourceMaterialView> material() const;

    [[nodiscard]] SemanticScene scene() const;

  private:
    friend class SemanticScene;
    SemanticNodeView(std::shared_ptr<const detail::SemanticSceneState> state,
                     const SemanticNode *node) noexcept;

    std::shared_ptr<const detail::SemanticSceneState> state_;
    const SemanticNode *node_{nullptr};
};

// ── Scene ─────────────────────────────────────────────────────────────────────

/// Whole-scene counts, computed once when the handle is created.
struct SemanticStats {
    std::size_t nodeCount{};
    std::size_t logicalVolumeCount{};
    std::size_t shapeCount{};
    std::size_t materialCount{};
    /// Nodes actually reachable from the root. Can be lower than `nodeCount`:
    /// a prune may leave detached nodes in the map.
    std::size_t reachableNodeCount{};
    std::size_t leafCount{};
    std::size_t booleanShapeCount{};
    int maxDepth{};
};

class SemanticScene {
  public:
    SemanticScene() noexcept;
    SemanticScene(const SemanticScene &);
    SemanticScene(SemanticScene &&) noexcept;
    SemanticScene &operator=(const SemanticScene &);
    SemanticScene &operator=(SemanticScene &&) noexcept;
    ~SemanticScene();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] std::string_view sourceFile() const noexcept;
    [[nodiscard]] SemanticNodeId rootId() const noexcept;
    [[nodiscard]] std::optional<SemanticNodeView> root() const;

    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] std::size_t logicalVolumeCount() const noexcept;
    [[nodiscard]] std::size_t shapeCount() const noexcept;
    [[nodiscard]] std::size_t materialCount() const noexcept;
    [[nodiscard]] const SemanticStats &stats() const noexcept;

    [[nodiscard]] std::optional<SemanticNodeView> node(SemanticNodeId id) const;
    [[nodiscard]] std::optional<LogicalVolumeView> logicalVolume(SemanticLogVolId id) const;
    [[nodiscard]] std::optional<ShapeView> shape(SemanticShapeId id) const;
    [[nodiscard]] std::optional<SourceMaterialView> material(SemanticMaterialId id) const;

    /// Stable orders that do not depend on the backing container: `nodeIds` is
    /// depth-first preorder from the root (so unreachable nodes are absent),
    /// the rest are ascending by id.
    [[nodiscard]] std::span<const SemanticNodeId> nodeIds() const noexcept;
    [[nodiscard]] std::span<const SemanticLogVolId> logicalVolumeIds() const noexcept;
    [[nodiscard]] std::span<const SemanticShapeId> shapeIds() const noexcept;
    [[nodiscard]] std::span<const SemanticMaterialId> materialIds() const noexcept;

    /// One step of a traversal. `siblingIndex`/`siblingCount` and
    /// `isLastSibling` are what a tree printer needs to draw its prefix, which
    /// is why they are here rather than left for each caller to recompute.
    struct Visit {
        SemanticNodeView node;
        int depth{0};
        std::size_t siblingIndex{0};
        std::size_t siblingCount{1};
        bool isLastSibling{true};
    };

    /// Return false from `fn` to skip that node's subtree.
    ///
    /// Depth-first preorder, children in stored order, and safe on a cyclic or
    /// partially-pruned graph. Deliberately not a template: a header template
    /// cannot carry an export annotation, and separate instantiations either
    /// side of a shared-library boundary break type identity.
    using Visitor = std::function<bool(const Visit &)>;
    void traverse(const Visitor &fn) const;
    void traverseFrom(SemanticNodeId start, const Visitor &fn) const;

  private:
    friend class SemanticNodeView;
    friend SemanticScene wrapSemanticScene(std::shared_ptr<const detail::SemanticScene>);
    friend const std::shared_ptr<const detail::SemanticScene> &
    unwrapSemanticScene(const SemanticScene &) noexcept;

    explicit SemanticScene(std::shared_ptr<const detail::SemanticSceneState> state) noexcept;

    std::shared_ptr<const detail::SemanticSceneState> state_;
};

// ── Seam ──────────────────────────────────────────────────────────────────────
// Not part of the stable surface; only nodehammer's own sources call these.

[[nodiscard]] SemanticScene wrapSemanticScene(std::shared_ptr<const detail::SemanticScene> scene);

/// Convenience for producers holding a scene by value, which is how every
/// importer builds one. Freezes by move.
[[nodiscard]] SemanticScene wrapSemanticScene(detail::SemanticScene scene);

[[nodiscard]] const std::shared_ptr<const detail::SemanticScene> &
unwrapSemanticScene(const SemanticScene &handle) noexcept;

} // namespace nodehammer

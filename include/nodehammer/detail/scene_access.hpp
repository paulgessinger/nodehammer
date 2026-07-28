#pragma once

// Per-node access to a semantic scene. **Not public API.**
//
// The public surface (<nodehammer/scene.hpp>) covers conversion, diagnostics
// and whole-scene inspection. Walking the tree node by node is a different
// thing: it exposes the shape of the IR, and every accessor here is a promise
// about structure that the opaque handle exists precisely to avoid making.
//
// In-tree consumers — the CLI in particular — are free to use this. It lives
// under detail/ so that choice is visible at the include line rather than
// implied, and so nothing outside the repo can depend on it by accident.
//
// The views are free functions over a scene handle rather than members of it,
// because members would have to be declared in the public header and would
// therefore be public whether or not anyone wanted them to be.

#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/scene.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace nodehammer::detail {

class LogicalVolumeView;
class ShapeView;
class SourceMaterialView;

/// The shape variants, collapsed to a flat enum, so a caller can ask what kind
/// of solid a shape is without visiting a thirteen-way variant. The parameters
/// stay unavailable on purpose.
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

/// Lowercase and stable: "box", "tube", ..., "union", "unknown".
[[nodiscard]] std::string_view shapeKindName(ShapeKind kind) noexcept;

[[nodiscard]] constexpr bool isBoolean(ShapeKind kind) noexcept {
    return kind == ShapeKind::Union || kind == ShapeKind::Intersection ||
           kind == ShapeKind::Subtraction;
}

// ── Views ─────────────────────────────────────────────────────────────────────
//
// Each holds shared ownership of the scene, so a view may outlive the handle it
// came from and the string_views it returns stay valid for as long as it does.

class SourceMaterialView {
  public:
    [[nodiscard]] SemanticMaterialId id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] double density() const noexcept;
    [[nodiscard]] std::optional<glm::vec3> color() const noexcept;

    SourceMaterialView(std::shared_ptr<const SemanticScene> scene,
                       const SourceMaterial *material) noexcept;

  private:
    std::shared_ptr<const SemanticScene> scene_;
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

    [[nodiscard]] std::optional<SemanticShapeId> booleanLeft() const noexcept;
    [[nodiscard]] std::optional<SemanticShapeId> booleanRight() const noexcept;

    /// Triangle count for a tessellated shape; 0 for every other kind.
    [[nodiscard]] std::size_t triangleCount() const noexcept;

    ShapeView(std::shared_ptr<const SemanticScene> scene, const SemanticShape *shape) noexcept;

  private:
    std::shared_ptr<const SemanticScene> scene_;
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

    LogicalVolumeView(std::shared_ptr<const SemanticScene> scene,
                      const SemanticLogicalVolume *logVol) noexcept;

  private:
    std::shared_ptr<const SemanticScene> scene_;
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

    SemanticNodeView(std::shared_ptr<const SemanticScene> scene, const SemanticNode *node) noexcept;

  private:
    std::shared_ptr<const SemanticScene> scene_;
    const SemanticNode *node_{nullptr};
};

// ── Lookup and traversal ──────────────────────────────────────────────────────

[[nodiscard]] std::optional<SemanticNodeView> node(const nodehammer::SemanticScene &scene,
                                                   SemanticNodeId id);
[[nodiscard]] std::optional<SemanticNodeView> root(const nodehammer::SemanticScene &scene);
[[nodiscard]] std::optional<LogicalVolumeView> logicalVolume(const nodehammer::SemanticScene &scene,
                                                             SemanticLogVolId id);
[[nodiscard]] std::optional<ShapeView> shape(const nodehammer::SemanticScene &scene,
                                             SemanticShapeId id);
[[nodiscard]] std::optional<SourceMaterialView> material(const nodehammer::SemanticScene &scene,
                                                         SemanticMaterialId id);

/// Stable orders that do not depend on the backing container: `nodeIds` is
/// depth-first preorder from the root (so unreachable nodes are absent), the
/// rest are ascending by id.
[[nodiscard]] std::span<const SemanticNodeId> nodeIds(const nodehammer::SemanticScene &scene);
[[nodiscard]] std::span<const SemanticLogVolId>
logicalVolumeIds(const nodehammer::SemanticScene &scene);
[[nodiscard]] std::span<const SemanticShapeId> shapeIds(const nodehammer::SemanticScene &scene);
[[nodiscard]] std::span<const SemanticMaterialId>
materialIds(const nodehammer::SemanticScene &scene);

/// One step of a traversal. `siblingIndex`/`siblingCount` and `isLastSibling`
/// are what a tree printer needs to draw its prefix, which is why they are here
/// rather than left for each caller to recompute.
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
/// partially-pruned graph.
using Visitor = std::function<bool(const Visit &)>;
void traverse(const nodehammer::SemanticScene &scene, const Visitor &fn);
void traverseFrom(const nodehammer::SemanticScene &scene, SemanticNodeId start, const Visitor &fn);

} // namespace nodehammer::detail

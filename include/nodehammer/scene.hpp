#pragma once

// Public handle over the semantic IR.
//
// A handle is a value type on the outside and a shared_ptr on the inside: it
// copies cheaply, it can be held across calls, and it keeps the scene alive for
// exactly as long as someone is looking at it.
//
// The surface here is deliberately whole-scene. Consumers hold a scene between
// verbs — import it, select, tessellate, export, serialize — and ask summary
// questions about it. They do not walk it node by node: that would expose the
// shape of the IR, which is the thing the handle exists to keep private.
// In-tree code that genuinely needs per-node access reaches for
// <nodehammer/detail/scene_access.hpp> and accepts what that implies.
//
// Handles are read-only. The pipeline mutates scenes while building them (prep
// copies its inputs precisely so the caller's scene is not touched); once a
// scene is wrapped it is frozen, and every public verb takes handles and
// returns new ones.
//
// This header is on the amalgamated header's C++20 floor and must stay clear of
// nlohmann. glm and unordered_dense are fine — both are shimmed there.

#include <nodehammer/ir/ids.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

class SemanticScene;

namespace detail {
class SemanticScene;
struct SemanticSceneState;

// Declared here only so the handle can befriend them; defined in
// <nodehammer/detail/handle_seam.hpp>, which is where callers get them.
nodehammer::SemanticScene wrapSemanticScene(std::shared_ptr<const SemanticScene>);
const std::shared_ptr<const SemanticScene> &
unwrapSemanticScene(const nodehammer::SemanticScene &) noexcept;
const std::shared_ptr<const SemanticSceneState> &
unwrapSemanticSceneState(const nodehammer::SemanticScene &) noexcept;
} // namespace detail

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

/// A shape-kind tally, e.g. {"box", 692}, ordered by name. The counterpart of
/// asking "what is in this file" without being handed the geometry itself.
struct ShapeKindCount {
    std::string kind;
    std::size_t count{};
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

    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] std::size_t logicalVolumeCount() const noexcept;
    [[nodiscard]] std::size_t shapeCount() const noexcept;
    [[nodiscard]] std::size_t materialCount() const noexcept;
    [[nodiscard]] const SemanticStats &stats() const noexcept;

    /// Shape kinds present, with counts, ordered by kind name.
    [[nodiscard]] std::vector<ShapeKindCount> shapeKindCounts() const;

    /// Source material names, ordered by material id — a stable order, unlike
    /// the backing map's, which varies with the container and standard library.
    [[nodiscard]] std::vector<std::string> materialNames() const;

  private:
    friend struct detail::SemanticSceneState;
    friend SemanticScene detail::wrapSemanticScene(std::shared_ptr<const detail::SemanticScene>);
    friend const std::shared_ptr<const detail::SemanticScene> &
    detail::unwrapSemanticScene(const SemanticScene &) noexcept;
    friend const std::shared_ptr<const detail::SemanticSceneState> &
    detail::unwrapSemanticSceneState(const SemanticScene &) noexcept;

    explicit SemanticScene(std::shared_ptr<const detail::SemanticSceneState> state) noexcept;

    std::shared_ptr<const detail::SemanticSceneState> state_;
};

} // namespace nodehammer

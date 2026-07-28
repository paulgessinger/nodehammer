#pragma once

// Public handles over the render IR.
//
// A handle is a value type on the outside and a shared_ptr on the inside: it
// copies cheaply, it can be held across calls, and it keeps the scene alive for
// exactly as long as someone is looking at it. Consumers never see the storage
// layout — which is the point, since it lets the containers, field set and
// iteration order behind these accessors change freely.
//
// Handles are read-only. The pipeline mutates scenes in place while building
// them (prep copies its inputs precisely so the caller's scene is not touched);
// once a scene is wrapped it is frozen, and every public verb takes handles and
// returns new ones.
//
// The one deliberate exception to opacity is the vertex buffer: Vertex's layout
// is pinned by schemas/render.fbs, so handing out spans over those exact bytes
// commits to nothing that .nhr files do not already commit to.

#include <nodehammer/ir/render_vocab.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace nodehammer {

class RenderScene;

namespace detail {
struct MeshAsset;
struct RenderScene;
struct RenderSceneState;

// Declared here only so the handle can befriend them; defined in
// <nodehammer/detail/handle_seam.hpp>.
nodehammer::RenderScene wrapRenderScene(std::shared_ptr<const RenderScene>);
const std::shared_ptr<const RenderScene> &
unwrapRenderScene(const nodehammer::RenderScene &) noexcept;
} // namespace detail

// ── Mesh ──────────────────────────────────────────────────────────────────────

/// A single mesh asset within a scene.
///
/// Copying shares ownership of the whole scene, so a MeshView may outlive the
/// RenderScene handle it came from — the spans it hands out stay valid for as
/// long as the view does.
class MeshView {
  public:
    [[nodiscard]] MeshAssetId id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;

    [[nodiscard]] std::size_t vertexCount() const noexcept;
    [[nodiscard]] std::size_t indexCount() const noexcept;
    [[nodiscard]] std::size_t triangleCount() const noexcept;

    /// Zero-copy views over the mesh's own storage. Valid while this MeshView
    /// (or any handle sharing the same scene) is alive.
    [[nodiscard]] std::span<const Vertex> vertices() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> indices() const noexcept;

    /// The same bytes, but owning: the returned pointer keeps the scene alive
    /// on its own, with no copy and no extra allocation. For handing a buffer
    /// to an async upload or a foreign API that outlives this view.
    ///
    /// Note it retains the *whole* scene, not just this mesh.
    [[nodiscard]] std::shared_ptr<const Vertex[]> ownedVertices() const;
    [[nodiscard]] std::shared_ptr<const std::uint32_t[]> ownedIndices() const;

    /// The scene this mesh belongs to.
    [[nodiscard]] RenderScene scene() const;

  private:
    friend class RenderScene;
    MeshView(std::shared_ptr<const detail::RenderSceneState> state,
             const detail::MeshAsset *mesh) noexcept;

    std::shared_ptr<const detail::RenderSceneState> state_;
    const detail::MeshAsset *mesh_{nullptr};
};

// ── Scene ─────────────────────────────────────────────────────────────────────

class RenderScene {
  public:
    /// An empty handle. `valid()` is false; every accessor returns a zero value
    /// or nullopt rather than trapping.
    RenderScene() noexcept;
    RenderScene(const RenderScene &);
    RenderScene(RenderScene &&) noexcept;
    RenderScene &operator=(const RenderScene &);
    RenderScene &operator=(RenderScene &&) noexcept;
    ~RenderScene();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] RenderNodeId rootId() const noexcept;

    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] std::size_t meshCount() const noexcept;
    [[nodiscard]] std::size_t materialCount() const noexcept;
    /// Summed over mesh assets, counting each asset once regardless of how many
    /// nodes instance it.
    [[nodiscard]] std::size_t triangleCount() const noexcept;

    /// Nullopt when no mesh carries that id.
    [[nodiscard]] std::optional<MeshView> mesh(MeshAssetId id) const;
    [[nodiscard]] std::optional<RenderMaterial> material(RenderMaterialId id) const;

    /// Mesh ids in ascending id order — deliberately *not* map order, which
    /// varies with the container, the standard library and the amalgamated
    /// header's unordered_dense shim.
    [[nodiscard]] std::span<const MeshAssetId> meshIds() const noexcept;

  private:
    friend class MeshView;
    friend RenderScene detail::wrapRenderScene(std::shared_ptr<const detail::RenderScene>);
    friend const std::shared_ptr<const detail::RenderScene> &
    detail::unwrapRenderScene(const RenderScene &) noexcept;

    explicit RenderScene(std::shared_ptr<const detail::RenderSceneState> state) noexcept;

    std::shared_ptr<const detail::RenderSceneState> state_;
};

} // namespace nodehammer

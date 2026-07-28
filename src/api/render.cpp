#include <nodehammer/render.hpp>

#include "state.hpp"

#include <algorithm>
#include <utility>

namespace nodehammer {

namespace detail {

RenderSceneState::RenderSceneState(std::shared_ptr<const RenderScene> s) : scene(std::move(s)) {
    if (!scene) {
        return;
    }

    meshIds.reserve(scene->meshAssets.size());
    for (const auto &[id, mesh] : scene->meshAssets) {
        meshIds.push_back(id);
        triangleCount += mesh.indices.size() / 3;
    }
    std::ranges::sort(meshIds, [](MeshAssetId a, MeshAssetId b) { return a.value < b.value; });
}

} // namespace detail

// ── MeshView ──────────────────────────────────────────────────────────────────

MeshView::MeshView(std::shared_ptr<const detail::RenderSceneState> state,
                   const detail::MeshAsset *mesh) noexcept
    : state_(std::move(state)), mesh_(mesh) {}

MeshAssetId MeshView::id() const noexcept { return mesh_->id; }

std::string_view MeshView::name() const noexcept { return mesh_->name; }

std::size_t MeshView::vertexCount() const noexcept { return mesh_->vertices.size(); }

std::size_t MeshView::indexCount() const noexcept { return mesh_->indices.size(); }

std::size_t MeshView::triangleCount() const noexcept { return mesh_->indices.size() / 3; }

std::span<const Vertex> MeshView::vertices() const noexcept { return mesh_->vertices; }

std::span<const std::uint32_t> MeshView::indices() const noexcept { return mesh_->indices; }

std::shared_ptr<const Vertex[]> MeshView::ownedVertices() const {
    // Aliasing constructor: shares the state's control block while pointing at
    // the vertex array. No copy, no allocation, and the scene stays alive for
    // as long as the returned pointer does.
    return {state_, mesh_->vertices.data()};
}

std::shared_ptr<const std::uint32_t[]> MeshView::ownedIndices() const {
    return {state_, mesh_->indices.data()};
}

RenderScene MeshView::scene() const { return RenderScene{state_}; }

// ── RenderScene ───────────────────────────────────────────────────────────────

RenderScene::RenderScene() noexcept = default;
RenderScene::RenderScene(const RenderScene &) = default;
RenderScene::RenderScene(RenderScene &&) noexcept = default;
RenderScene &RenderScene::operator=(const RenderScene &) = default;
RenderScene &RenderScene::operator=(RenderScene &&) noexcept = default;
RenderScene::~RenderScene() = default;

RenderScene::RenderScene(std::shared_ptr<const detail::RenderSceneState> state) noexcept
    : state_(std::move(state)) {}

bool RenderScene::valid() const noexcept { return state_ != nullptr && state_->scene != nullptr; }

RenderNodeId RenderScene::rootId() const noexcept {
    return valid() ? state_->scene->rootId : RenderNodeId{};
}

std::size_t RenderScene::nodeCount() const noexcept {
    return valid() ? state_->scene->nodes.size() : 0;
}

std::size_t RenderScene::meshCount() const noexcept {
    return valid() ? state_->scene->meshAssets.size() : 0;
}

std::size_t RenderScene::materialCount() const noexcept {
    return valid() ? state_->scene->materials.size() : 0;
}

std::size_t RenderScene::triangleCount() const noexcept {
    return valid() ? state_->triangleCount : 0;
}

std::optional<MeshView> RenderScene::mesh(MeshAssetId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto it = state_->scene->meshAssets.find(id);
    if (it == state_->scene->meshAssets.end()) {
        return std::nullopt;
    }
    return MeshView{state_, &it->second};
}

std::optional<RenderMaterial> RenderScene::material(RenderMaterialId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto it = state_->scene->materials.find(id);
    if (it == state_->scene->materials.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::span<const MeshAssetId> RenderScene::meshIds() const noexcept {
    return state_ ? std::span<const MeshAssetId>{state_->meshIds} : std::span<const MeshAssetId>{};
}

// ── Seam ──────────────────────────────────────────────────────────────────────

RenderScene wrapRenderScene(std::shared_ptr<const detail::RenderScene> scene) {
    if (!scene) {
        return RenderScene{};
    }
    return RenderScene{std::make_shared<const detail::RenderSceneState>(std::move(scene))};
}

const std::shared_ptr<const detail::RenderScene> &
unwrapRenderScene(const RenderScene &handle) noexcept {
    static const std::shared_ptr<const detail::RenderScene> empty;
    return handle.state_ ? handle.state_->scene : empty;
}

} // namespace nodehammer

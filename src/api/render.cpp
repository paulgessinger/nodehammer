#include <nodehammer/render.hpp>

#include <nodehammer/detail/handle_seam.hpp>
#include <nodehammer/ir/render.hpp>

#include <algorithm>
#include <utility>

namespace nodehammer {

// ── MeshView ──────────────────────────────────────────────────────────────────

MeshView::MeshView(std::shared_ptr<const detail::RenderScene> scene,
                   const detail::MeshAsset *mesh) noexcept
    : scene_(std::move(scene)), mesh_(mesh) {}

MeshAssetId MeshView::id() const noexcept { return mesh_->id; }

std::string_view MeshView::name() const noexcept { return mesh_->name; }

std::size_t MeshView::vertexCount() const noexcept { return mesh_->vertices.size(); }

std::size_t MeshView::indexCount() const noexcept { return mesh_->indices.size(); }

std::size_t MeshView::triangleCount() const noexcept { return mesh_->indices.size() / 3; }

std::span<const Vertex> MeshView::vertices() const noexcept {
    // The storage form keeps glm; the public form is plain floats. They are
    // layout-identical by static_assert (ir/render.hpp), so this hands back the
    // scene's own bytes rather than a conversion — which is the whole point.
    return {reinterpret_cast<const Vertex *>(mesh_->vertices.data()), mesh_->vertices.size()};
}

std::span<const std::uint32_t> MeshView::indices() const noexcept { return mesh_->indices; }

std::shared_ptr<const Vertex[]> MeshView::ownedVertices() const {
    // Aliasing constructor: shares the state's control block while pointing at
    // the vertex array. No copy, no allocation, and the scene stays alive for
    // as long as the returned pointer does.
    return {scene_, reinterpret_cast<const Vertex *>(mesh_->vertices.data())};
}

std::shared_ptr<const std::uint32_t[]> MeshView::ownedIndices() const {
    return {scene_, mesh_->indices.data()};
}

RenderScene MeshView::scene() const { return RenderScene{scene_}; }

// ── RenderScene ───────────────────────────────────────────────────────────────

RenderScene::RenderScene() noexcept = default;
RenderScene::RenderScene(const RenderScene &) = default;
RenderScene::RenderScene(RenderScene &&) noexcept = default;
RenderScene &RenderScene::operator=(const RenderScene &) = default;
RenderScene &RenderScene::operator=(RenderScene &&) noexcept = default;
RenderScene::~RenderScene() = default;

RenderScene::RenderScene(std::shared_ptr<const detail::RenderScene> scene) noexcept
    : scene_(std::move(scene)) {}

bool RenderScene::valid() const noexcept { return scene_ != nullptr; }

RenderNodeId RenderScene::rootId() const noexcept {
    return valid() ? scene_->rootId : RenderNodeId{};
}

std::size_t RenderScene::nodeCount() const noexcept { return valid() ? scene_->nodes.size() : 0; }

std::size_t RenderScene::meshCount() const noexcept {
    return valid() ? scene_->meshAssets.size() : 0;
}

std::size_t RenderScene::materialCount() const noexcept {
    return valid() ? scene_->materials.size() : 0;
}

std::size_t RenderScene::triangleCount() const noexcept {
    if (!valid()) {
        return 0;
    }
    std::size_t total = 0;
    for (const auto &[id, mesh] : scene_->meshAssets) {
        total += mesh.indices.size() / 3;
    }
    return total;
}

std::optional<MeshView> RenderScene::mesh(MeshAssetId id) const {
    if (!valid()) {
        return std::nullopt;
    }
    const auto it = scene_->meshAssets.find(id);
    if (it == scene_->meshAssets.end()) {
        return std::nullopt;
    }
    return MeshView{scene_, &it->second};
}

std::vector<MeshAssetId> RenderScene::meshIds() const {
    if (!valid()) {
        return {};
    }
    std::vector<MeshAssetId> ids;
    ids.reserve(scene_->meshAssets.size());
    for (const auto &[id, mesh] : scene_->meshAssets) {
        ids.push_back(id);
    }
    std::ranges::sort(ids, [](MeshAssetId a, MeshAssetId b) { return a.value < b.value; });
    return ids;
}

namespace detail {

// ── Seam ──────────────────────────────────────────────────────────────────────

nodehammer::RenderScene wrapRenderScene(std::shared_ptr<const RenderScene> scene) {
    return nodehammer::RenderScene{std::move(scene)};
}

const std::shared_ptr<const RenderScene> &
unwrapRenderScene(const nodehammer::RenderScene &handle) noexcept {
    return handle.scene_;
}

} // namespace detail

} // namespace nodehammer

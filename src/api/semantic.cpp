#include <nodehammer/scene.hpp>

#include <nodehammer/detail/handle_seam.hpp>
#include <nodehammer/detail/scene_access.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace nodehammer {

SemanticScene::SemanticScene() noexcept = default;
SemanticScene::SemanticScene(const SemanticScene &) = default;
SemanticScene::SemanticScene(SemanticScene &&) noexcept = default;
SemanticScene &SemanticScene::operator=(const SemanticScene &) = default;
SemanticScene &SemanticScene::operator=(SemanticScene &&) noexcept = default;
SemanticScene::~SemanticScene() = default;

SemanticScene::SemanticScene(std::shared_ptr<const detail::SemanticScene> scene) noexcept
    : scene_(std::move(scene)) {}

bool SemanticScene::valid() const noexcept { return scene_ != nullptr; }

std::string_view SemanticScene::sourceFile() const noexcept {
    return valid() ? std::string_view{scene_->sourceFile} : std::string_view{};
}

std::size_t SemanticScene::nodeCount() const noexcept { return valid() ? scene_->nodes.size() : 0; }

std::size_t SemanticScene::logicalVolumeCount() const noexcept {
    return valid() ? scene_->logVols.size() : 0;
}

std::size_t SemanticScene::shapeCount() const noexcept {
    return valid() ? scene_->shapes.size() : 0;
}

std::size_t SemanticScene::materialCount() const noexcept {
    return valid() ? scene_->materials.size() : 0;
}

std::vector<ShapeKindCount> SemanticScene::shapeKindCounts() const {
    if (!valid()) {
        return {};
    }
    // std::map so the result is name-ordered without a second sort.
    std::map<std::string, std::size_t> tally;
    for (const auto &[id, shape] : scene_->shapes) {
        ++tally[std::string{
            detail::shapeKindName(static_cast<detail::ShapeKind>(shape.data.index()))}];
    }

    std::vector<ShapeKindCount> counts;
    counts.reserve(tally.size());
    for (auto &[kind, count] : tally) {
        counts.push_back(ShapeKindCount{kind, count});
    }
    return counts;
}

std::vector<std::string> SemanticScene::materialNames() const {
    if (!valid()) {
        return {};
    }
    std::vector<SemanticMaterialId> ids;
    ids.reserve(scene_->materials.size());
    for (const auto &[id, material] : scene_->materials) {
        ids.push_back(id);
    }
    std::ranges::sort(ids, [](auto a, auto b) { return a.value < b.value; });

    std::vector<std::string> names;
    names.reserve(ids.size());
    for (const auto id : ids) {
        names.push_back(scene_->materials.at(id).name);
    }
    return names;
}

namespace detail {

// ── Seam ──────────────────────────────────────────────────────────────────────

nodehammer::SemanticScene wrapSemanticScene(std::shared_ptr<const SemanticScene> scene) {
    return nodehammer::SemanticScene{std::move(scene)};
}

nodehammer::SemanticScene wrapSemanticScene(SemanticScene scene) {
    return wrapSemanticScene(std::make_shared<const SemanticScene>(std::move(scene)));
}

const std::shared_ptr<const SemanticScene> &
unwrapSemanticScene(const nodehammer::SemanticScene &handle) noexcept {
    return handle.scene_;
}

} // namespace detail

} // namespace nodehammer

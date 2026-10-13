#pragma once

#include <ir/expanded/scene.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <tessellation/occurrence_overrides.hpp>

namespace nodehammer::tessellation {

// Random access to preorder occurrence IDs without storing an expanded node tree.
// Only prototype subtree sizes and daughter offsets are retained. IDs match expand().
class OccurrenceNodes {
  public:
    explicit OccurrenceNodes(const ir::semantic::Scene &scene,
                             const OccurrenceOverrides *overrides = nullptr)
        : scene_(scene), count_(scene.nodeCount()), overrides_(overrides) {
        if (count_ == 0) {
            return;
        }
        tree_ = scene.makeTree(0);
        // Iterative postorder: deep source hierarchies must not exhaust the C++ stack.
        std::vector<std::pair<ir::semantic::LogVolId, bool>> pending{{scene.root.logVolId, false}};
        while (!pending.empty()) {
            const auto [id, ready] = pending.back();
            pending.pop_back();
            if (sizes_.contains(id)) {
                continue;
            }
            const auto &daughters = scene.logVols.at(id).daughters;
            if (!ready) {
                pending.emplace_back(id, true);
                for (const auto &daughter : daughters) {
                    if (!sizes_.contains(daughter.logVolId)) {
                        pending.emplace_back(daughter.logVolId, false);
                    }
                }
                continue;
            }
            uint64_t size = 1;
            auto &offsets = offsets_[id];
            for (const auto &daughter : daughters) {
                offsets.push_back(size);
                size += sizes_.at(daughter.logVolId);
            }
            sizes_.emplace(id, size);
        }
    }

    [[nodiscard]] bool contains(ir::expanded::NodeId id) const {
        return id.value > 0 && id.value <= count_ && (!overrides_ || !overrides_->excludes(id));
    }
    [[nodiscard]] uint64_t size() const { return count_; }

    struct Geometry {
        ir::semantic::LogVolId volume;
        glm::dmat4 world{1.0};
        ir::expanded::NodeId parent;
    };
    // Geometry-only lookup: no names, tags, paths or child vectors are allocated.
    // Deliberately ignores overrides, which never change the source graph.
    [[nodiscard]] Geometry geometryAt(ir::expanded::NodeId id, bool transforms = true) const {
        if (id.value == 0 || id.value > count_) {
            throw std::out_of_range("invalid wedge occurrence ID");
        }
        Geometry out{scene_.root.logVolId, scene_.root.localTransform, {}};
        uint64_t base = 1;
        while (base != id.value) {
            const auto &offsets = offsets_.at(out.volume);
            const auto it = std::upper_bound(offsets.begin(), offsets.end(), id.value - base);
            const auto index = static_cast<std::size_t>(it - offsets.begin() - 1);
            out.parent = ir::expanded::NodeId{base};
            base += offsets.at(index);
            const auto &daughter = scene_.logVols.at(out.volume).daughters.at(index);
            out.volume = daughter.logVolId;
            if (transforms) {
                out.world *= daughter.localTransform;
            }
        }
        return out;
    }

    [[nodiscard]] glm::dmat4 childLocalTransform(const ir::expanded::Node &parent,
                                                 std::size_t index) const {
        auto volume = parent.logVolId;
        if (overrides_ && overrides_->volumes.contains(parent.id)) {
            volume = geometryAt(parent.id, false).volume;
        }
        if (overrides_ && !overrides_->pruned.empty()) {
            // The surviving child list may have holes in the source daughters.
            const auto offset = parent.children.at(index).value - parent.id.value;
            const auto &offsets = offsets_.at(volume);
            index = static_cast<std::size_t>(
                std::lower_bound(offsets.begin(), offsets.end(), offset) - offsets.begin());
        }
        return scene_.logVols.at(volume).daughters.at(index).localTransform;
    }

    [[nodiscard]] ir::expanded::Node at(ir::expanded::NodeId id) const {
        if (!contains(id)) {
            throw std::out_of_range("invalid tessellation occurrence ID");
        }
        ir::expanded::Node node;
        node.id = id;
        node.name = scene_.root.name;
        node.localTransform = scene_.root.localTransform;
        node.worldTransform = scene_.root.localTransform;
        node.originalPath = "/" + std::string{tree_->pathNameFor({}, scene_.root.name)};
        ir::semantic::OccurrenceId path;
        auto volume = scene_.root.logVolId;
        uint64_t base = 1;
        std::optional<ir::semantic::PlacementKey> placement;
        while (base != id.value) {
            const auto &offsets = offsets_.at(volume);
            const auto it = std::upper_bound(offsets.begin(), offsets.end(), id.value - base);
            const auto index = static_cast<std::size_t>(it - offsets.begin() - 1);
            node.parentId = ir::expanded::NodeId{base};
            placement = ir::semantic::PlacementKey{volume, index};
            base += offsets.at(index);
            path.push_back(index);
            const auto &daughter = scene_.logVols.at(volume).daughters.at(index);
            volume = daughter.logVolId;
            node.name = daughter.name;
            node.localTransform = daughter.localTransform;
            node.worldTransform *= daughter.localTransform;
            node.originalPath += "/";
            node.originalPath += tree_->pathNameFor(path, daughter.name);
        }
        node.logVolId = volume;
        node.name = tree_->displayNameFor(path, node.name);
        node.originalPath = tree_->originalPathFor(path, node.originalPath);
        node.tags = tree_->tagsFor(path, volume, placement);
        node.sourceSystem = tree_->sourceSystemFor(path);
        if (const auto it = scene_.metadata.degradation.find(path);
            it != scene_.metadata.degradation.end()) {
            node.degradation = it->second;
        }
        for (const auto offset : offsets_.at(volume)) {
            const ir::expanded::NodeId child{base + offset};
            if (!overrides_ || !overrides_->excludes(child)) {
                node.children.push_back(child);
            }
        }
        if (overrides_) {
            if (const auto it = overrides_->volumes.find(id); it != overrides_->volumes.end()) {
                node.logVolId = it->second;
            }
        }
        return node;
    }

  private:
    const ir::semantic::Scene &scene_;
    uint64_t count_;
    const OccurrenceOverrides *overrides_;
    std::unique_ptr<ir::semantic::OccurrenceTree> tree_;
    ankerl::unordered_dense::map<ir::semantic::LogVolId, uint64_t> sizes_;
    ankerl::unordered_dense::map<ir::semantic::LogVolId, std::vector<uint64_t>> offsets_;
};

} // namespace nodehammer::tessellation

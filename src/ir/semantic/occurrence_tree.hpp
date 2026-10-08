#pragma once

#include <ir/semantic.hpp>

#include <list>
#include <memory>
#include <stdexcept>

namespace nodehammer::ir::semantic {

struct Occurrence {
    OccurrenceId id;
    LogVolId logVolId;
    std::string name;
    std::string originalPath;
    std::string materialName;
    std::string sourceSystem;
    DegradationFlags degradation;
    glm::dmat4 localTransform{1.0};
    glm::dmat4 worldTransform{1.0};
    OccurrenceTags tags;
    std::size_t childCount{};
};

// Lazy view over canonical source daughter placements. The caller must keep source alive and
// immutable. In particular, do not pass a pruned scene and expect its original daughter closure to
// describe the selected tree. Older deduplicated files may have already lost daughter names;
// current name-preserving deduplication cannot repair that information. Only a lossless source
// prototype graph is valid. These preconditions are NOT inferred or checked from GeometryCatalogs.
// No expanded nodes are consulted or created here.
//
// capacity limits cache-owned entries, not bytes or externally pinned snapshots.
// Returned shared_ptrs pin immutable snapshots across eviction. setTags replaces
// persistent occurrence tags; previously pinned snapshots retain their old tags.
// Persistent tags and external pins are not bounded by the cache capacity.
// Not thread safe. Shared import/processing creates a tree per operation.
class OccurrenceTree {
  public:
    OccurrenceTree(const GeometryCatalogs &source, DaughterPlacement root, std::size_t capacity)
        : source_(source), root_(std::move(root)), capacity_(capacity) {
        (void)source_.logVols.at(root_.logVolId);
    }
    OccurrenceTree(const OccurrenceTree &) = delete;
    OccurrenceTree &operator=(const OccurrenceTree &) = delete;

    [[nodiscard]] std::shared_ptr<const Occurrence> materialize(const OccurrenceId &id) {
        if (auto it = cache_.find(id); it != cache_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.position);
            ++hits_;
            return it->second.value;
        }
        auto value = std::make_shared<Occurrence>(resolve(id));
        ++materializations_;
        if (capacity_ != 0) {
            lru_.push_front(id);
            try {
                cache_.emplace(id, Entry{value, lru_.begin()});
            } catch (...) {
                lru_.pop_front();
                throw;
            }
            while (cache_.size() > capacity_) {
                cache_.erase(lru_.back());
                lru_.pop_back();
            }
        }
        return value;
    }

    void setOriginalPath(const OccurrenceId &id, std::string path) {
        (void)locate(id);
        originalPaths_.insert_or_assign(id, std::move(path));
        invalidate(id);
    }
    [[nodiscard]] std::string_view originalPathFor(const OccurrenceId &id,
                                                   std::string_view fallback) const {
        auto it = originalPaths_.find(id);
        return it == originalPaths_.end() ? fallback : std::string_view{it->second};
    }
    void setDegradation(const OccurrenceId &id, DegradationFlags flags) {
        (void)locate(id);
        degradation_.insert_or_assign(id, flags);
        invalidate(id);
    }
    void setDefaultSourceSystem(std::string system) {
        defaultSourceSystem_ = std::move(system);
        clearCache();
    }
    void setSourceSystem(const OccurrenceId &id, std::string system) {
        (void)locate(id);
        sourceSystems_.insert_or_assign(id, std::move(system));
        invalidate(id);
    }
    [[nodiscard]] std::string_view sourceSystemFor(const OccurrenceId &id) const {
        auto it = sourceSystems_.find(id);
        return it == sourceSystems_.end() ? std::string_view{defaultSourceSystem_}
                                          : std::string_view{it->second};
    }

    // Import-time semantic path segments are separate from later display renames.
    void setPathName(const OccurrenceId &id, std::string name) {
        (void)locate(id);
        pathNames_.insert_or_assign(id, std::move(name));
        clearCache(); // Descendant paths also change.
    }
    [[nodiscard]] std::string_view pathNameFor(const OccurrenceId &id,
                                               std::string_view fallback) const {
        auto it = pathNames_.find(id);
        return it == pathNames_.end() ? fallback : std::string_view{it->second};
    }

    // Display names never alter original source-path segments.
    void setDisplayName(const OccurrenceId &id, std::string name) {
        (void)locate(id);
        displayNames_.insert_or_assign(id, std::move(name));
        invalidate(id);
    }
    void clearDisplayNameOverride(const OccurrenceId &id) {
        (void)locate(id);
        displayNames_.erase(id);
        invalidate(id);
    }
    [[nodiscard]] std::string_view displayNameFor(const OccurrenceId &id,
                                                  std::string_view sourceName) const {
        auto it = displayNames_.find(id);
        return it == displayNames_.end() ? sourceName : std::string_view{it->second};
    }

    // Defaults are reusable across all occurrences of a volume or daughter
    // placement. Placement keys override volume keys. Exact occurrence overrides
    // replace the complete effective map (including an explicit empty map).
    void setVolumeTags(LogVolId volume, OccurrenceTags tags) {
        (void)source_.logVols.at(volume);
        const auto value = intern(std::move(tags));
        volumeTags_.insert_or_assign(volume, value);
        clearCache();
    }
    void setPlacementTags(LogVolId parent, std::size_t daughter, OccurrenceTags tags) {
        (void)source_.logVols.at(parent).daughters.at(daughter);
        const auto value = intern(std::move(tags));
        placementTags_.insert_or_assign(std::pair{parent, daughter}, value);
        clearCache();
    }
    void setTags(const OccurrenceId &id, OccurrenceTags tags) {
        (void)locate(id); // Validate before writing persistent state.
        const auto value = intern(std::move(tags));
        tags_.insert_or_assign(id, value);
        invalidate(id);
    }
    void clearTagOverride(const OccurrenceId &id) {
        (void)locate(id);
        tags_.erase(id);
        invalidate(id);
    }

    [[nodiscard]] const GeometryCatalogs &sourceScene() const { return source_; }
    [[nodiscard]] const DaughterPlacement &rootPlacement() const { return root_; }
    // Configure metadata before constructing selection views. All configuration
    // is immutable while views are used; the lazily merged immutable tag sets
    // themselves are interned once per distinct default combination.
    [[nodiscard]] const OccurrenceTags &tagsFor(const OccurrenceId &id) const {
        const auto [volume, placement] = locate(id);
        return tagsFor(id, volume, placement);
    }
    // Fast path for a traversal which already resolved this source identity.
    [[nodiscard]] const OccurrenceTags &
    tagsFor(const OccurrenceId &id, LogVolId volume,
            std::optional<std::pair<LogVolId, std::size_t>> placement = std::nullopt) const {
        if (auto it = tags_.find(id); it != tags_.end()) {
            return *tagSets_.at(it->second);
        }
        const auto vol = volumeTags_.find(volume);
        const auto edge = placement ? placementTags_.find(*placement) : placementTags_.end();
        if (vol == volumeTags_.end() && edge == placementTags_.end()) {
            static const OccurrenceTags empty;
            return empty;
        }
        if (vol == volumeTags_.end()) {
            return *tagSets_.at(edge->second);
        }
        if (edge == placementTags_.end()) {
            return *tagSets_.at(vol->second);
        }
        const auto key = std::pair{vol->second, edge->second};
        if (auto it = mergedTags_.find(key); it != mergedTags_.end()) {
            return *tagSets_.at(it->second);
        }
        auto combined = *tagSets_.at(vol->second);
        for (const auto &[name, value] : *tagSets_.at(edge->second)) {
            combined.insert_or_assign(name, value);
        }
        const auto value = intern(std::move(combined));
        mergedTags_.emplace(key, value);
        return *tagSets_.at(value);
    }
    [[nodiscard]] std::size_t internedTagSetCount() const { return tagSets_.size(); }
    [[nodiscard]] std::size_t tagOverrideCount() const { return tags_.size(); }
    [[nodiscard]] std::size_t volumeTagDefaultCount() const { return volumeTags_.size(); }
    [[nodiscard]] std::size_t placementTagDefaultCount() const { return placementTags_.size(); }

    void clearCache() {
        cache_.clear();
        lru_.clear();
    }
    [[nodiscard]] std::size_t cachedCount() const { return cache_.size(); }
    [[nodiscard]] std::size_t materializationCount() const { return materializations_; }
    [[nodiscard]] std::size_t cacheHitCount() const { return hits_; }

    // Depth-first traversal with O(depth) traversal state. Returning false skips
    // children, but does NOT prescribe selection semantics (e.g. hoisting may
    // require visiting children of a dropped parent). Callback references live
    // only for that call; retain materialize(id) explicitly to pin a snapshot.
    template <class Visitor> void visit(Visitor &&visitor) {
        struct Frame {
            std::size_t nextChild{};
            std::size_t childCount{};
        };
        auto root = materialize({});
        if (!visitor(*root)) {
            return;
        }
        std::vector<Frame> stack{{0, root->childCount}};
        OccurrenceId id;
        root.reset();
        while (!stack.empty()) {
            auto &frame = stack.back();
            if (frame.nextChild == frame.childCount) {
                stack.pop_back();
                if (!id.empty()) {
                    id.pop_back();
                }
                continue;
            }
            id.push_back(frame.nextChild++);
            auto value = materialize(id);
            if (visitor(*value)) {
                stack.push_back({0, value->childCount});
            } else {
                id.pop_back();
            }
        }
    }

  private:
    using PlacementKey = std::pair<LogVolId, std::size_t>;
    [[nodiscard]] std::pair<LogVolId, std::optional<PlacementKey>>
    locate(const OccurrenceId &id) const {
        auto volume = root_.logVolId;
        std::optional<PlacementKey> placement;
        std::unordered_set<LogVolId> ancestors{volume};
        for (const auto daughter : id) {
            placement = PlacementKey{volume, daughter};
            volume = source_.logVols.at(volume).daughters.at(daughter).logVolId;
            if (!ancestors.insert(volume).second) {
                throw std::invalid_argument("cycle in occurrence prototype ancestry");
            }
        }
        (void)source_.logVols.at(volume);
        return {volume, placement};
    }
    [[nodiscard]] std::size_t intern(OccurrenceTags tags) const {
        const auto [it, added] = tagPool_.try_emplace(std::move(tags), tagSets_.size());
        if (added) {
            try {
                tagSets_.push_back(&it->first);
            } catch (...) {
                tagPool_.erase(it);
                throw;
            }
        }
        return it->second;
    }
    void invalidate(const OccurrenceId &id) {
        if (auto it = cache_.find(id); it != cache_.end()) {
            lru_.erase(it->second.position);
            cache_.erase(it);
        }
    }

    [[nodiscard]] Occurrence resolve(const OccurrenceId &id) const {
        Occurrence result;
        result.id = id;
        result.name = root_.name;
        result.originalPath = "/" + std::string{pathNameFor({}, root_.name)};
        OccurrenceId prefix;
        result.logVolId = root_.logVolId;
        result.localTransform = root_.localTransform;
        result.worldTransform = root_.localTransform;
        std::unordered_set<LogVolId> ancestors{root_.logVolId};
        for (const auto index : id) {
            const auto &daughter = source_.logVols.at(result.logVolId).daughters.at(index);
            if (!ancestors.insert(daughter.logVolId).second) {
                throw std::invalid_argument("cycle in occurrence prototype ancestry");
            }
            result.logVolId = daughter.logVolId;
            result.name = daughter.name;
            prefix.push_back(index);
            result.originalPath += "/";
            result.originalPath += pathNameFor(prefix, daughter.name);
            result.localTransform = daughter.localTransform;
            result.worldTransform *= daughter.localTransform;
        }
        const auto &volume = source_.logVols.at(result.logVolId);
        result.childCount = volume.daughters.size();
        if (auto it = source_.materials.find(volume.materialId); it != source_.materials.end()) {
            result.materialName = it->second.name;
        }
        result.tags = tagsFor(id);
        result.name = displayNameFor(id, result.name);
        result.sourceSystem = sourceSystemFor(id);
        result.originalPath = originalPathFor(id, result.originalPath);
        if (auto it = degradation_.find(id); it != degradation_.end())
            result.degradation = it->second;
        return result;
    }

    struct Entry {
        std::shared_ptr<const Occurrence> value;
        std::list<OccurrenceId>::iterator position;
    };
    const GeometryCatalogs &source_;
    DaughterPlacement root_;
    std::size_t capacity_;
    std::list<OccurrenceId> lru_;
    std::map<OccurrenceId, Entry> cache_;
    std::map<OccurrenceId, std::size_t> tags_;
    std::map<OccurrenceId, std::string> displayNames_;
    std::map<OccurrenceId, std::string> pathNames_;
    std::map<OccurrenceId, std::string> originalPaths_;
    std::map<OccurrenceId, DegradationFlags> degradation_;
    std::string defaultSourceSystem_;
    std::map<OccurrenceId, std::string> sourceSystems_;
    std::map<LogVolId, std::size_t> volumeTags_;
    std::map<PlacementKey, std::size_t> placementTags_;
    // std::map key addresses are stable. Cache eviction never invalidates tag
    // references used by lightweight filter views. Interned definitions live for
    // the tree lifetime; callers must not accumulate unbounded metadata edits.
    mutable std::map<OccurrenceTags, std::size_t> tagPool_;
    mutable std::vector<const OccurrenceTags *> tagSets_;
    mutable std::map<std::pair<std::size_t, std::size_t>, std::size_t> mergedTags_;
    std::size_t materializations_{};
    std::size_t hits_{};
};

} // namespace nodehammer::ir::semantic

#include <ir/expanded/conversion.hpp>
#include <selection/occurrence_selector.hpp>
#include <selection/selector.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <bit>
#include <optional>
#include <unordered_map>

namespace nodehammer::ir::semantic {
namespace {

// Projection visits source routes in lexicographic DFS order. Each sparse map
// therefore needs one advancing cursor, not a lookup or an allocated path entry
// for every retained occurrence. Skipped subtrees are consumed by the next call.
template <class Map> class MetadataCursor {
  public:
    explicit MetadataCursor(const Map &map) : map_(map), next_(map.begin()) {}
    const typename Map::mapped_type *at(const OccurrenceId &route) {
        while (next_ != map_.end() && next_->first < route)
            ++next_;
        return next_ != map_.end() && next_->first == route ? &next_->second : nullptr;
    }

  private:
    const Map &map_;
    typename Map::const_iterator next_;
};

void combineHash(std::size_t &seed, std::size_t value) {
    seed ^= value + std::size_t{0x9e3779b9} + (seed << 6) + (seed >> 2);
}

bool sameTransform(const glm::dmat4 &left, const glm::dmat4 &right) {
    for (int col = 0; col != 4; ++col)
        for (int row = 0; row != 4; ++row)
            if (std::bit_cast<uint64_t>(left[col][row]) != std::bit_cast<uint64_t>(right[col][row]))
                return false;
    return true;
}

using TagDefault = std::optional<uint64_t>;
TagDefault placementDefault(const Metadata &metadata, LogVolId parent, std::size_t daughter) {
    const auto it = metadata.placementTags.find({parent, daughter});
    return it == metadata.placementTags.end() ? std::nullopt : TagDefault{it->second};
}

// The original volume is part of the identity: this preserves its volume-tag
// default, catalog name and all shape/material references without inventing new
// cross-volume deduplication semantics. Incoming occurrence context stays on the
// placement or in sparse occurrence metadata, not in the definition cache.
class Definitions {
  public:
    Definitions(const Scene &source, Scene &output) : source_(source), output_(output) {}

    LogVolId intern(LogVolId original, std::vector<DaughterPlacement> &daughters,
                    const std::vector<TagDefault> &defaults) {
        std::size_t hash = std::hash<uint64_t>{}(original.value);
        for (std::size_t i = 0; i != daughters.size(); ++i) {
            const auto &daughter = daughters[i];
            combineHash(hash, std::hash<std::string>{}(daughter.name));
            combineHash(hash, std::hash<uint64_t>{}(daughter.logVolId.value));
            for (int col = 0; col != 4; ++col)
                for (int row = 0; row != 4; ++row)
                    combineHash(hash, std::hash<uint64_t>{}(std::bit_cast<uint64_t>(
                                          daughter.localTransform[col][row])));
            combineHash(hash, defaults[i].has_value());
            if (defaults[i])
                combineHash(hash, std::hash<uint64_t>{}(*defaults[i]));
        }
        auto &bucket = buckets_[hash];
        for (const auto &[sourceId, resultId] : bucket) {
            if (sourceId != original)
                continue;
            const auto &candidate = output_.logVols.at(resultId).daughters;
            if (candidate.size() != daughters.size())
                continue;
            bool equal = true;
            for (std::size_t i = 0; i != candidate.size(); ++i) {
                if (candidate[i].name != daughters[i].name ||
                    candidate[i].logVolId != daughters[i].logVolId ||
                    !sameTransform(candidate[i].localTransform, daughters[i].localTransform) ||
                    placementDefault(output_.metadata, resultId, i) != defaults[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal)
                return resultId;
        }
        const auto &source = source_.logVols.at(original);
        const auto id = output_.nextLogVolId();
        output_.logVols.emplace(id, LogicalVolume{id, source.name, source.shapeId,
                                                  source.materialId, std::move(daughters)});
        if (const auto it = source_.metadata.volumeTags.find(original);
            it != source_.metadata.volumeTags.end())
            output_.metadata.volumeTags.emplace(id, it->second);
        for (std::size_t i = 0; i != defaults.size(); ++i)
            if (defaults[i])
                output_.metadata.placementTags.emplace(PlacementKey{id, i}, *defaults[i]);
        bucket.emplace_back(original, id);
        return id;
    }

  private:
    const Scene &source_;
    Scene &output_;
    std::unordered_map<std::size_t, std::vector<std::pair<LogVolId, LogVolId>>> buckets_;
};

} // namespace

DiagnosticList select(Scene &scene, const std::vector<config::SelectionRule> &rules, bool hoist) {
    if (scene.nodeCount() == 0)
        return {};
    auto tree = scene.makeTree(0);
    const selection::SelectedOccurrences original{*tree};
    const auto selected = original.prune(rules, hoist);
    auto diags = selected.diagnostics();
    if (diags.hasErrors())
        return diags;
    if (selected.size() == original.size()) {
        scene.collectGarbage();
        return diags;
    }

    Scene result;
    result.sourceFile = scene.sourceFile;
    result.shapes = scene.shapes;
    result.materials = scene.materials;
    result.metadata.tagSets = scene.metadata.tagSets;
    result.metadata.defaultSourceSystem = scene.metadata.defaultSourceSystem;
    Definitions definitions{scene, result};
    MetadataCursor tags{scene.metadata.occurrenceTags};
    MetadataCursor names{scene.metadata.displayNames};
    MetadataCursor pathNames{scene.metadata.pathNames};
    MetadataCursor paths{scene.metadata.originalPaths};
    MetadataCursor systems{scene.metadata.sourceSystems};
    MetadataCursor degradation{scene.metadata.degradation};

    struct Frame {
        const DaughterPlacement *placement;
        std::optional<std::size_t> keptParent;
        TagDefault incomingTags;
        glm::dmat4 world{1.0};
        glm::dmat4 local{1.0};
        std::vector<DaughterPlacement> daughters;
        std::vector<TagDefault> daughterTags;
        // Source path segments between the nearest retained parent and a
        // dropped node. Only hoisted roots need to store the composed prefix.
        std::string droppedPath;
        std::size_t next{};
        bool entered{};
        bool kept{};
        Frame(const DaughterPlacement *edge, std::optional<std::size_t> parent, TagDefault tags,
              const glm::dmat4 &worldTransform, const glm::dmat4 &localTransform)
            : placement(edge), keptParent(parent), incomingTags(tags), world(worldTransform),
              local(localTransform) {}
    };
    std::vector<Frame> stack{{&scene.root, std::nullopt, std::nullopt, scene.root.localTransform,
                              scene.root.localTransform}};
    OccurrenceId sourceRoute, outputRoute;
    while (!stack.empty()) {
        auto &frame = stack.back();
        const auto volume = frame.placement->logVolId;
        const auto &daughters = scene.logVols.at(volume).daughters;
        if (!frame.entered) {
            const auto retained = selected.subtreeSize(sourceRoute);
            if (retained == 0) {
                stack.pop_back();
                if (!stack.empty())
                    sourceRoute.pop_back();
                continue;
            }
            frame.entered = true;
            frame.kept = selected.contains(sourceRoute);
            const auto *pathName = pathNames.at(sourceRoute);
            const std::string &segment = pathName ? *pathName : frame.placement->name;
            const bool hoisted = frame.keptParent && *frame.keptParent + 1 != stack.size() - 1;
            if (frame.kept) {
                if (frame.keptParent)
                    outputRoute.push_back(stack[*frame.keptParent].daughters.size());
                if (hoisted) {
                    frame.local = glm::affineInverse(stack[*frame.keptParent].world) * frame.world;
                    // pathNames are arbitrary imported path strings. Joining the
                    // dropped segments at this root preserves descendant paths
                    // too, without changing raw placement or display names.
                    result.metadata.pathNames.emplace(
                        outputRoute, stack[stack.size() - 2].droppedPath + "/" + segment);
                } else if (pathName)
                    result.metadata.pathNames.emplace(outputRoute, *pathName);
                if (const auto *value = tags.at(sourceRoute))
                    result.metadata.occurrenceTags.emplace(outputRoute, *value);
                if (const auto *value = names.at(sourceRoute))
                    result.metadata.displayNames.emplace(outputRoute, *value);
                if (const auto *value = paths.at(sourceRoute))
                    result.metadata.originalPaths.emplace(outputRoute, *value);
                if (const auto *value = systems.at(sourceRoute))
                    result.metadata.sourceSystems.emplace(outputRoute, *value);
                if (const auto *value = degradation.at(sourceRoute))
                    result.metadata.degradation.emplace(outputRoute, *value);
                // A retained leaf may have millions of removed descendants.
                if (retained == 1)
                    frame.next = daughters.size();
            } else {
                if (stack.size() > 1 && !stack[stack.size() - 2].kept)
                    frame.droppedPath = stack[stack.size() - 2].droppedPath + "/" + segment;
                else
                    frame.droppedPath = segment;
            }
        }
        if (frame.next != daughters.size()) {
            const auto index = frame.next++;
            const auto &child = daughters[index];
            const auto parent = frame.kept ? std::optional{stack.size() - 1} : frame.keptParent;
            const auto world = frame.world * child.localTransform;
            sourceRoute.push_back(index);
            stack.push_back({&child, parent, placementDefault(scene.metadata, volume, index), world,
                             child.localTransform});
            continue;
        }
        if (frame.kept) {
            const auto id = definitions.intern(volume, frame.daughters, frame.daughterTags);
            DaughterPlacement placement{frame.placement->name, id, frame.local};
            if (frame.keptParent) {
                auto &parent = stack[*frame.keptParent];
                parent.daughters.push_back(std::move(placement));
                parent.daughterTags.push_back(frame.incomingTags);
                outputRoute.pop_back();
            } else
                result.root = std::move(placement);
        }
        stack.pop_back();
        if (!stack.empty())
            sourceRoute.pop_back();
    }
    result.collectGarbage();
    result.reseedIdCounters();
    scene = std::move(result);
    return diags;
}
} // namespace nodehammer::ir::semantic

namespace nodehammer::selection {
DiagnosticList SelectionEngine::prune(ir::semantic::Scene &scene) const {
    return ir::semantic::select(scene, rules_, hoistOrphans_);
}
} // namespace nodehammer::selection

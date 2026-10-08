#include <diagnostic_codes.hpp>
#include <ir/dd4hep/semantic/shared_importer.hpp>
#include <ir/tgeo/semantic/shared_importer.hpp>

#include <DD4hep/DetElement.h>
#include <DD4hep/Detector.h>
#include <DD4hep/Volumes.h>

#include <TGeoManager.h>
#include <TGeoNode.h>
#include <TGeoVolume.h>

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer::ir::semantic {
namespace {
using semantic::LogVolId;

struct Route {
    unsigned count{}; // Capped at two: zero, unique, ambiguous.
    std::optional<PlacementKey> predecessor;
};

// Search backwards through unique source volumes, never through physical
// occurrences. Memoization is scoped to (anchor volume, destination volume).
// A unique result retains only its final edge, rather than a copied full path.
class Routes {
  public:
    explicit Routes(const GeometryCatalogs &source) {
        for (const auto &[id, volume] : source.logVols) {
            for (std::size_t i = 0; i < volume.daughters.size(); ++i) {
                incoming_[volume.daughters.at(i).logVolId].emplace_back(id, i);
            }
        }
    }

    [[nodiscard]] const Route &find(LogVolId anchor, LogVolId target) {
        const auto key = std::pair{anchor, target};
        if (auto it = memo_.find(key); it != memo_.end()) {
            return it->second;
        }
        struct Frame {
            LogVolId volume;
            std::size_t next{};
            Route route;
        };
        std::vector<Frame> stack{{target, 0, {}}};
        std::unordered_set<LogVolId> active{target};
        while (!stack.empty()) {
            auto &frame = stack.back();
            const auto parents = incoming_.find(frame.volume);
            if (frame.volume == anchor || parents == incoming_.end() || frame.route.count == 2 ||
                frame.next == parents->second.size()) {
                if (frame.volume == anchor) {
                    frame.route = {1, std::nullopt};
                }
                memo_.emplace(std::pair{anchor, frame.volume}, frame.route);
                active.erase(frame.volume);
                stack.pop_back();
                continue;
            }
            const auto edge = parents->second.at(frame.next);
            const auto parentKey = std::pair{anchor, edge.first};
            const auto parent = memo_.find(parentKey);
            if (parent == memo_.end()) {
                if (!active.insert(edge.first).second) {
                    throw std::invalid_argument("cycle in source placement ancestry");
                }
                stack.push_back({edge.first, 0, {}});
                continue;
            }
            if (parent->second.count != 0) {
                if (frame.route.count == 0 && parent->second.count == 1) {
                    frame.route.predecessor = edge;
                } else {
                    frame.route.predecessor.reset();
                }
                frame.route.count = std::min(2u, frame.route.count + parent->second.count);
            }
            ++frame.next;
        }
        return memo_.at(key);
    }

    [[nodiscard]] OccurrenceId uniquePath(LogVolId anchor, LogVolId target) const {
        OccurrenceId reversed;
        while (target != anchor) {
            const auto &route = memo_.at(std::pair{anchor, target});
            if (route.count != 1 || !route.predecessor) {
                throw std::logic_error("requested an ambiguous source placement path");
            }
            reversed.push_back(route.predecessor->second);
            target = route.predecessor->first;
        }
        std::reverse(reversed.begin(), reversed.end());
        return reversed;
    }

  private:
    std::unordered_map<LogVolId, std::vector<PlacementKey>> incoming_;
    std::map<std::pair<LogVolId, LogVolId>, Route> memo_;
};

struct Anchor {
    OccurrenceId id;
    const TGeoNode *node;
    LogVolId volume;
};

struct Resolution {
    unsigned count{};
    OccurrenceId relative;
};

Resolution resolve(const Anchor &anchor, const TGeoNode *target,
                   const detail::TGeoSourceIndex &source, Routes &routes) {
    if (target == anchor.node) {
        return {1, {}};
    }
    const auto found = source.placements.find(target);
    if (found == source.placements.end()) {
        return {};
    }
    Resolution result;
    std::optional<PlacementKey> unique;
    for (const auto &edge : found->second) {
        const auto count = routes.find(anchor.volume, edge.first).count;
        if (count == 0) {
            continue;
        }
        unique = result.count == 0 && count == 1 ? std::optional{edge} : std::nullopt;
        result.count = std::min(2u, result.count + count);
        if (result.count == 2) {
            break;
        }
    }
    if (unique) {
        result.relative = routes.uniquePath(anchor.volume, unique->first);
        result.relative.push_back(unique->second);
    }
    return result;
}

class TagInterner {
  public:
    explicit TagInterner(Metadata &metadata) : metadata_(metadata) {}
    uint64_t intern(OccurrenceTags tags) {
        if (auto it = ids_.find(tags); it != ids_.end()) {
            return it->second;
        }
        const auto id = static_cast<uint64_t>(metadata_.tagSets.size());
        metadata_.tagSets.push_back(tags);
        ids_.emplace(std::move(tags), id);
        return id;
    }

  private:
    Metadata &metadata_;
    std::map<OccurrenceTags, uint64_t> ids_;
};

} // namespace

ir::ImportResult importDD4hep(dd4hep::Detector &detector, std::string sourceFile) {
    auto source = detail::extractTGeoSource(&detector.manager(), std::move(sourceFile));
    auto &geometry = source.result.scene;
    auto &metadata = geometry.metadata;
    auto &diags = source.result.diags;
    metadata.defaultSourceSystem = "dd4hep/tgeo";
    TagInterner tags{metadata};
    for (const auto &[volume, id] : source.volumes) {
        if (dd4hep::Volume{const_cast<TGeoVolume *>(volume)}.isSensitive()) {
            metadata.volumeTags.emplace(id, tags.intern({{"sensitive", "true"}}));
        }
    }

    Routes routes{geometry};
    struct Frame {
        dd4hep::DetElement element;
        Anchor anchor;
    };
    std::vector<Frame> pending{{detector.world(), {{}, source.rootNode, geometry.root.logVolId}}};
    while (!pending.empty()) {
        auto frame = std::move(pending.back());
        pending.pop_back();
        const auto &element = frame.element;
        auto next = std::move(frame.anchor);
        const auto placement = element.placement();
        if (!placement.isValid()) {
            diags.warn(codes::kWarnImportUnplacedAncestor,
                       std::format("DetElement '{}' has no placement; resolving children from "
                                   "the nearest placed ancestor '{}'",
                                   element.name(), next.node->GetName()),
                       element.path());
        } else {
            const auto resolved = resolve(next, placement.ptr(), source, routes);
            if (resolved.count != 1) {
                diags.error(codes::kErrImportPlacementUnresolved,
                            std::format("DetElement '{}' placement '{}' has {} source route "
                                        "from ancestor '{}'; metadata was not assigned",
                                        element.name(), placement->GetName(),
                                        resolved.count == 0 ? "no" : "more than one",
                                        next.node->GetName()),
                            element.path());
            } else {
                next.id.insert(next.id.end(), resolved.relative.begin(), resolved.relative.end());
                next.node = placement.ptr();
                next.volume = source.volumes.at(placement->GetVolume());
                OccurrenceTags defaults;
                if (auto it = metadata.volumeTags.find(next.volume);
                    it != metadata.volumeTags.end()) {
                    defaults = metadata.tagSets.at(static_cast<std::size_t>(it->second));
                }
                auto complete = defaults;
                if (!element.type().empty()) {
                    complete.insert_or_assign("subdetector", element.type());
                }
                const auto previousName = metadata.displayNames.find(next.id);
                if (previousName != metadata.displayNames.end()) {
                    auto previous = defaults;
                    if (auto it = metadata.occurrenceTags.find(next.id);
                        it != metadata.occurrenceTags.end()) {
                        previous = metadata.tagSets.at(static_cast<std::size_t>(it->second));
                    }
                    if (previousName->second != element.name() || previous != complete) {
                        diags.error(codes::kErrImportPlacementUnresolved,
                                    std::format("DetElement '{}' conflicts with metadata for '{}' "
                                                "on the same source occurrence; keeping the first "
                                                "annotation only",
                                                element.name(), previousName->second),
                                    element.path());
                    }
                } else {
                    metadata.displayNames.emplace(next.id, element.name());
                    metadata.pathNames.emplace(next.id, element.name());
                    metadata.sourceSystems.emplace(next.id, "dd4hep");
                    if (complete != defaults) {
                        metadata.occurrenceTags.emplace(next.id, tags.intern(std::move(complete)));
                    }
                }
            }
        }
        // Reverse insertion keeps the DetElement map's visitation order without
        // recursive C++ calls. An unplaced/ambiguous ancestor does not hide children.
        for (auto it = element.children().rbegin(); it != element.children().rend(); ++it) {
            pending.push_back({it->second, next});
        }
    }
    const auto occurrences = geometry.validate();
    diags.debug(codes::kDebugImportStats,
                std::format("{} source volume(s), {} source shape(s), {} physical occurrence(s), "
                            "{} explicit DetElement occurrence(s); no expanded Nodes",
                            geometry.logVols.size(), geometry.shapes.size(), occurrences,
                            metadata.displayNames.size()),
                "dd4hep/shared");
    return std::move(source.result);
}

} // namespace nodehammer::ir::semantic

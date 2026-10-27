#include <algorithm>
#include <config/predicate_parser.hpp>
#include <detail/overloaded.hpp>
#include <diagnostic_codes.hpp>
#include <geometry/subtract_daughters.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <map>
#include <selection/predicate.hpp>
#include <tuple>

namespace nodehammer::geometry {
namespace {
using namespace ir::semantic;
constexpr auto marker = "nodehammer_subtract_daughters";
struct Rule {
    selection::Predicate match;
    const config::Rule::Tessellation *settings;
};
// Only these fields are properties of a logical-volume definition. Names,
// paths and effective tags can differ between occurrences of that definition.
bool definitionOnly(const config::PredicateExpr &expr) {
    return std::visit(
        detail::overloaded{
            [](const config::VolumeGlobPredicate &) { return true; },
            [](const config::MaterialGlobPredicate &) { return true; },
            [](const config::BoolPredicate &) { return true; },
            [](const config::IsLeafPredicate &) { return true; },
            [](const std::shared_ptr<config::AndPredicate> &p) {
                return std::all_of(p->operands.begin(), p->operands.end(),
                                   [](const auto &operand) { return definitionOnly(operand); });
            },
            [](const std::shared_ptr<config::OrPredicate> &p) {
                return std::all_of(p->operands.begin(), p->operands.end(),
                                   [](const auto &operand) { return definitionOnly(operand); });
            },
            [](const std::shared_ptr<config::NotPredicate> &p) {
                return definitionOnly(p->operand);
            },
            [](const auto &) { return false; }},
        expr.data);
}
bool definitionOnly(const std::optional<std::string> &selector) {
    if (!selector || selector->empty())
        return true;
    const auto parsed = config::parsePredicateExpr(*selector);
    return parsed && definitionOnly(*parsed);
}
class Pass {
  public:
    Pass(const Scene &source, const config::NHConfig &cfg)
        : source_(source), output_(source), tree_(source.makeTree(0)),
          defaults_(cfg.tessellationDefaults) {
        output_.reseedIdCounters();
        memoize_ = definitionOnly(defaults_.subtractDaughtersMatch);
        for (const auto &r : cfg.rules)
            if (r.tessellation &&
                (r.tessellation->subtractDaughters || r.tessellation->subtractDaughtersMatch)) {
                memoize_ = memoize_ && (!r.match || definitionOnly(*r.match)) &&
                           definitionOnly(r.tessellation->subtractDaughtersMatch);
                rules_.push_back({r.match ? selection::compilePredicate(*r.match)
                                          : selection::Predicate{[](const auto &) { return true; }},
                                  &*r.tessellation});
            }
        // A baked pass marker can itself make occurrences differ, independently
        // of the user's selectors. Keep the occurrence path in that uncommon case.
        if (!source.metadata.occurrenceTags.empty() || !source.metadata.placementTags.empty())
            for (const auto &tags : source.metadata.tagSets)
                if (tags.contains(marker))
                    memoize_ = false;
    }
    DiagnosticList run(Scene &destination) {
        OccurrenceId route;
        output_.root.logVolId =
            visit(source_.root, route, "/" + std::string{tree_->pathNameFor({}, source_.root.name)},
                  std::nullopt);
        if (changed_) {
            // Memoized shared subtrees were visited only once. Preserve sparse
            // occurrence overrides and augment every affected override with its
            // new volume's marker, including paths bypassed by the memoization.
            if (memoize_) {
                for (auto &[overrideRoute, tagSet] : output_.metadata.occurrenceTags) {
                    auto volume = output_.root.logVolId;
                    for (const auto index : overrideRoute)
                        volume = output_.logVols.at(volume).daughters.at(index).logVolId;
                    const auto it = output_.metadata.volumeTags.find(volume);
                    if (it == output_.metadata.volumeTags.end())
                        continue;
                    const auto &volumeTags = output_.metadata.tagSets.at(it->second);
                    if (!volumeTags.contains(marker))
                        continue;
                    auto augmented = output_.metadata.tagSets.at(tagSet);
                    augmented[marker] = volumeTags.at(marker);
                    tagSet = output_.metadata.tagSets.size();
                    output_.metadata.tagSets.push_back(std::move(augmented));
                }
            }
            output_.collectGarbage();
            (void)output_.validate();
            destination = std::move(output_);
        }
        return std::move(diags_);
    }

  private:
    selection::NodeView view(const DaughterPlacement &edge, const OccurrenceId &route,
                             const std::string &path, const OccurrenceTags &tags) const {
        const auto &v = source_.logVols.at(edge.logVolId);
        return {tree_->displayNameFor(route, edge.name),
                tree_->originalPathFor(route, path),
                source_.materials.at(v.materialId).name,
                v.daughters.empty(),
                &tags,
                v.name};
    }
    LogVolId visit(const DaughterPlacement &edge, OccurrenceId &route, const std::string &path,
                   std::optional<PlacementKey> placement) {
        if (memoize_) {
            if (const auto found = definitions_.find(edge.logVolId); found != definitions_.end())
                return found->second;
        }
        const auto result = visitImpl(edge, route, path, placement);
        if (memoize_)
            definitions_.emplace(edge.logVolId, result);
        return result;
    }
    LogVolId visitImpl(const DaughterPlacement &edge, OccurrenceId &route, const std::string &path,
                       std::optional<PlacementKey> placement) {
        const auto &volume = source_.logVols.at(edge.logVolId);
        if (volume.daughters.empty())
            return edge.logVolId;
        const auto tags = tree_->tagsFor(route, edge.logVolId, placement);
        const auto nv = view(edge, route, path, tags);
        bool enabled = defaults_.subtractDaughters.value_or(false);
        auto selector = defaults_.subtractDaughtersMatch;
        for (const auto &r : rules_) {
            if (!r.match(nv))
                continue;
            if (r.settings->subtractDaughters)
                enabled = *r.settings->subtractDaughters;
            if (r.settings->subtractDaughtersMatch)
                selector = r.settings->subtractDaughtersMatch;
        }
        const auto policy = selector && !selector->empty() ? *selector : "true";
        bool already = tags.contains(marker);
        if (enabled && already && tags.at(marker) != policy)
            diags_.warn(codes::kWarnDaughterSubtraction,
                        "daughter subtraction already applied with different selectors; reload "
                        "original geometry",
                        path);
        std::vector<std::size_t> operands;
        auto copy = volume;
        std::vector<uint64_t> children;
        for (std::size_t i = 0; i < volume.daughters.size(); ++i) {
            const auto &child = volume.daughters[i];
            route.push_back(i);
            const auto childPath = path + "/" + std::string{tree_->pathNameFor(route, child.name)};
            if (enabled && !already) {
                auto it = predicates_.find(policy);
                if (it == predicates_.end()) {
                    const auto parsed = config::parsePredicateExpr(policy);
                    if (!parsed)
                        throw Error{codes::kErrConfigParse, parsed.error()};
                    it = predicates_.emplace(policy, selection::compilePredicate(*parsed)).first;
                }
                const auto childTags =
                    tree_->tagsFor(route, child.logVolId, PlacementKey{volume.id, i});
                if (it->second(view(child, route, childPath, childTags)))
                    operands.push_back(i);
            }
            copy.daughters[i].logVolId = visit(child, route, childPath, PlacementKey{volume.id, i});
            children.push_back(copy.daughters[i].logVolId.value);
            route.pop_back();
        }
        const bool childChanged =
            !std::equal(copy.daughters.begin(), copy.daughters.end(), volume.daughters.begin(),
                        [](const auto &a, const auto &b) { return a.logVolId == b.logVolId; });
        if (operands.empty() && !childChanged)
            return edge.logVolId;
        changed_ = true;
        if (!operands.empty() && source_.metadata.occurrenceTags.contains(route)) {
            auto augmented = tags;
            augmented[marker] = policy;
            output_.metadata.occurrenceTags[route] = output_.metadata.tagSets.size();
            output_.metadata.tagSets.push_back(std::move(augmented));
        }
        const auto key = std::tuple{volume.id.value, operands, children};
        if (const auto it = clones_.find(key); it != clones_.end())
            return it->second;
        // All operands come from the immutable input graph, including daughters
        // whose own material is carved below. Never subtract a daughter's shell.
        for (const auto index : operands) {
            const auto &child = volume.daughters[index];
            const auto id = output_.nextShapeId();
            output_.shapes.emplace(
                id, Shape{id, BooleanSubtraction{copy.shapeId,
                                                 source_.logVols.at(child.logVolId).shapeId,
                                                 child.localTransform}});
            copy.shapeId = id;
        }
        copy.id = output_.nextLogVolId();
        const auto id = copy.id;
        output_.logVols.emplace(id, std::move(copy));
        clones_.emplace(key, id);
        OccurrenceTags volumeTags;
        if (const auto it = source_.metadata.volumeTags.find(volume.id);
            it != source_.metadata.volumeTags.end())
            volumeTags = source_.metadata.tagSets.at(it->second);
        if (!operands.empty())
            volumeTags[marker] = policy;
        if (!volumeTags.empty()) {
            output_.metadata.volumeTags[id] = output_.metadata.tagSets.size();
            output_.metadata.tagSets.push_back(std::move(volumeTags));
        }
        for (std::size_t i = 0; i < volume.daughters.size(); ++i)
            if (const auto it = source_.metadata.placementTags.find({volume.id, i});
                it != source_.metadata.placementTags.end())
                output_.metadata.placementTags[{id, i}] = it->second;
        return id;
    }
    const Scene &source_;
    Scene output_;
    std::unique_ptr<OccurrenceTree> tree_;
    config::Rule::Tessellation defaults_;
    std::vector<Rule> rules_;
    std::map<std::string, selection::Predicate> predicates_;
    std::map<std::tuple<uint64_t, std::vector<std::size_t>, std::vector<uint64_t>>, LogVolId>
        clones_;
    DiagnosticList diags_;
    bool changed_{};
    bool memoize_{};
    std::map<LogVolId, LogVolId> definitions_;
};
} // namespace
bool hasDaughterSubtraction(const config::NHConfig &cfg) {
    return cfg.tessellationDefaults.subtractDaughters.value_or(false) ||
           std::any_of(cfg.rules.begin(), cfg.rules.end(), [](const auto &r) {
               return r.tessellation && r.tessellation->subtractDaughters.value_or(false);
           });
}
DiagnosticList subtractDaughters(Scene &scene, const config::NHConfig &cfg) {
    if (!hasDaughterSubtraction(cfg) || scene.logVols.empty())
        return {};
    return Pass{scene, cfg}.run(scene);
}
} // namespace nodehammer::geometry

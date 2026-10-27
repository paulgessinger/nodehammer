#include <geometry/drift_chamber.hpp>

#include <config/predicate_parser.hpp>
#include <diagnostic_codes.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <selection/predicate.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

namespace nodehammer::geometry {
namespace {
using namespace ir::semantic;
struct Rule {
    selection::Predicate match;
    selection::Predicate gas;
    selection::Predicate wires;
    bool explicitGas{};
    bool explicitWires{};
    std::string policy;
};
selection::Predicate predicate(const std::optional<std::string> &text) {
    if (!text)
        return [](const auto &) { return false; };
    auto parsed = config::parsePredicateExpr(*text);
    if (!parsed)
        throw Error{codes::kErrConfigParse, parsed.error()};
    return selection::compilePredicate(*parsed);
}
bool wire(const ShapeVariant &shape) {
    const auto *tube = std::get_if<TubeShape>(&shape);
    // Conservative aspect ratio: full length / diameter >= 100. Hollow,
    // segmented, Boolean and non-finite cylinders remain ordinary geometry.
    return tube && tube->rMin == 0 && tube->rMax > 0 && std::isfinite(tube->rMax) &&
           std::isfinite(tube->dz) && tube->dz / tube->rMax >= 100 &&
           std::abs(tube->phiDelta - 2 * std::numbers::pi) < 1e-9;
}
bool solid(const ShapeVariant &shape) {
    if (const auto *mesh = std::get_if<TessellatedShape>(&shape))
        return !mesh->triangles.empty();
    return !std::holds_alternative<UnknownShape>(shape);
}
struct Counts {
    std::size_t envelopes{}, cells{}, wires{}, unsupported{}, gasCandidates{}, wireCandidates{};
};
class Pass {
  public:
    Pass(const Scene &scene, const config::NHConfig &cfg)
        : source_(scene), result_(scene), tree_(scene.makeTree(0)) {
        result_.reseedIdCounters();
        for (const auto &r : cfg.rules) {
            if (!r.geometry || !r.geometry->driftChamber)
                continue;
            const auto &o = *r.geometry->driftChamber;
            rules_.push_back(
                {r.match ? selection::compilePredicate(*r.match)
                         : selection::Predicate{[](const auto &) { return true; }},
                 predicate(o.gas), predicate(o.wires), o.gas.has_value(), o.wires.has_value(),
                 (o.gas ? std::to_string(o.gas->size()) + ":" + *o.gas : "auto") + "|" +
                     (o.wires ? std::to_string(o.wires->size()) + ":" + *o.wires : "auto")});
        }
    }
    DiagnosticList run(Scene &out) {
        OccurrenceId route;
        result_.root.logVolId =
            visit(source_.root, route, "/" + std::string{tree_->pathNameFor({}, source_.root.name)},
                  std::nullopt, nullptr, std::nullopt, nullptr);
        if (changed_) {
            result_.collectGarbage();
            (void)result_.validate();
            out = std::move(result_);
        }
        return std::move(diags_);
    }

  private:
    LogVolId visit(const DaughterPlacement &edge, OccurrenceId &route, const std::string &path,
                   std::optional<PlacementKey> placement, const Rule *active,
                   std::optional<MaterialId> gasParent, Counts *counts) {
        const auto &volume = source_.logVols.at(edge.logVolId);
        const auto &tags = placement ? tree_->tagsFor(route, edge.logVolId, *placement)
                                     : tree_->tagsFor(route, edge.logVolId);
        const selection::NodeView view{tree_->displayNameFor(route, edge.name),
                                       tree_->originalPathFor(route, path),
                                       source_.materials.at(volume.materialId).name,
                                       volume.daughters.empty(),
                                       &tags,
                                       volume.name};
        bool chamberRoot = false;
        Counts localCounts;
        if (!active) {
            for (const auto &r : rules_)
                if (r.match(view))
                    active = &r; // Last matching chamber rule wins.
            if (active) {
                if (tags.contains(kDriftChamberTag) && tags.at(kDriftChamberTag) == "v1") {
                    if (!tags.contains("nodehammer_drift_policy") ||
                        tags.at("nodehammer_drift_policy") != active->policy)
                        diags_.warn(codes::kWarnDriftChamber,
                                    "drift chamber was already optimized with different selectors; "
                                    "reload original geometry to change the optimization",
                                    path);
                    return edge.logVolId;
                }
                chamberRoot = true;
                counts = &localCounts;
            }
        }
        std::string role;
        bool suppress = false;
        if (active) {
            const auto &shape = source_.shapes.at(volume.shapeId).data;
            const bool gas = active->gas(view);
            const bool recognizedWire = wire(shape);
            const bool candidate = active->explicitWires ? active->wires(view) : recognizedWire;
            counts->gasCandidates += gas ? 1 : 0;
            counts->wireCandidates += candidate ? 1 : 0;
            if (gas && candidate) {
                ++counts->unsupported; // Overlapping classifications are ambiguous.
            } else if (gas && solid(shape)) {
                if (gasParent && *gasParent == volume.materialId) {
                    suppress = true;
                    role = "gas_cell";
                    ++counts->cells;
                } else {
                    role = "gas_envelope";
                    gasParent = volume.materialId;
                    ++counts->envelopes;
                }
            } else if (candidate) {
                if (recognizedWire) {
                    role = "wire";
                    ++counts->wires;
                } else {
                    ++counts->unsupported;
                }
            } else if (gas) {
                ++counts->unsupported;
            }
        }
        auto copy = volume;
        bool modified = suppress || !role.empty() || chamberRoot;
        for (std::size_t i = 0; i != volume.daughters.size(); ++i) {
            const auto &child = volume.daughters[i];
            route.push_back(i);
            const auto childPath = path + "/" + std::string{tree_->pathNameFor(route, child.name)};
            const auto replacement = visit(child, route, childPath, PlacementKey{volume.id, i},
                                           active, gasParent, counts);
            route.pop_back();
            copy.daughters[i].logVolId = replacement;
            modified = modified || replacement != child.logVolId;
        }
        if (!modified)
            return edge.logVolId;
        changed_ = true;
        if (suppress) {
            if (!empty_) {
                empty_ = result_.nextShapeId();
                result_.shapes.emplace(*empty_, Shape{*empty_, TessellatedShape{}});
            }
            copy.shapeId = *empty_;
        }
        // Reuse equivalent transformed prototypes: no per-wire occurrence tags
        // for ordinary shared geometry. Existing placement/occurrence metadata
        // stays authoritative and is augmented only where it already exists.
        std::vector<uint64_t> children;
        for (const auto &child : copy.daughters)
            children.push_back(child.logVolId.value);
        const auto key = std::tuple{volume.id.value, role,
                                    chamberRoot ? active->policy : std::string{}, children};
        LogVolId id;
        if (const auto existing = clones_.find(key); existing != clones_.end()) {
            id = existing->second;
        } else {
            copy.id = result_.nextLogVolId();
            id = copy.id;
            result_.logVols.emplace(id, std::move(copy));
            clones_.emplace(key, id);
            OccurrenceTags volumeTags;
            if (const auto it = source_.metadata.volumeTags.find(volume.id);
                it != source_.metadata.volumeTags.end())
                volumeTags = source_.metadata.tagSets.at(it->second);
            if (!role.empty())
                volumeTags[kDriftRoleTag] = role;
            if (chamberRoot) {
                volumeTags[kDriftChamberTag] = "v1";
                volumeTags["nodehammer_drift_policy"] = active->policy;
            }
            if (!volumeTags.empty()) {
                const auto index = result_.metadata.tagSets.size();
                result_.metadata.tagSets.push_back(std::move(volumeTags));
                result_.metadata.volumeTags[id] = index;
            }
            for (std::size_t i = 0; i != volume.daughters.size(); ++i)
                if (const auto it = source_.metadata.placementTags.find({volume.id, i});
                    it != source_.metadata.placementTags.end())
                    result_.metadata.placementTags[{id, i}] = it->second;
        }
        if ((!role.empty() || chamberRoot) && source_.metadata.occurrenceTags.contains(route)) {
            auto augmented = tags;
            if (!role.empty())
                augmented[kDriftRoleTag] = role;
            if (chamberRoot) {
                augmented[kDriftChamberTag] = "v1";
                augmented["nodehammer_drift_policy"] = active->policy;
            }
            const auto index = result_.metadata.tagSets.size();
            result_.metadata.tagSets.push_back(std::move(augmented));
            result_.metadata.occurrenceTags[route] = index;
        }
        if (chamberRoot) {
            diags_.info(codes::kInfoDriftChamber,
                        "drift chamber: retained " + std::to_string(counts->envelopes) +
                            " gas envelopes, suppressed " + std::to_string(counts->cells) +
                            " internal gas surfaces, recognized " + std::to_string(counts->wires) +
                            " analytic wire instances",
                        path);
            if (active->explicitGas && counts->gasCandidates == 0)
                diags_.warn(codes::kWarnDriftChamber, "gas predicate matched no nodes", path);
            if (active->explicitWires && counts->wireCandidates == 0)
                diags_.warn(codes::kWarnDriftChamber, "wire predicate matched no nodes", path);
            if (counts->envelopes == 0 && counts->wires == 0)
                diags_.warn(codes::kWarnDriftChamber,
                            "no supported gas envelopes or wires matched; geometry retained", path);
            if (!active->explicitGas)
                diags_.warn(codes::kWarnDriftChamber, "no gas predicate: gas surfaces retained",
                            path);
            if (counts->unsupported)
                diags_.warn(codes::kWarnDriftChamber,
                            std::to_string(counts->unsupported) +
                                " ambiguous or unsupported candidates retained",
                            path);
        }
        return id;
    }
    const Scene &source_;
    Scene result_;
    std::unique_ptr<OccurrenceTree> tree_;
    std::vector<Rule> rules_;
    DiagnosticList diags_;
    bool changed_{};
    std::optional<ShapeId> empty_;
    std::map<std::tuple<uint64_t, std::string, std::string, std::vector<uint64_t>>, LogVolId>
        clones_;
};
} // namespace
DiagnosticList optimizeDriftChambers(ir::semantic::Scene &scene, const config::NHConfig &cfg) {
    if (scene.logVols.empty() ||
        std::none_of(cfg.rules.begin(), cfg.rules.end(),
                     [](const auto &r) { return r.geometry && r.geometry->driftChamber; }))
        return {};
    return Pass{scene, cfg}.run(scene);
}
} // namespace nodehammer::geometry

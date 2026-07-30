#include <config/config_validator.hpp>
#include <ir/diagnostic_codes.hpp>

#include <format>
#include <unordered_set>

namespace nodehammer::config {

ir::DiagnosticList ConfigValidator::validate(const NHConfig &cfg) {
    ir::DiagnosticList diags;

    // Build set of defined material names for reference checking.
    std::unordered_set<std::string> definedMaterials;
    for (const auto &mat : cfg.materials) {
        definedMaterials.insert(mat.name);
    }

    bool sawEarlierMaterialRule = false;
    for (const auto &rule : cfg.rules) {
        // Every referenced material must be defined.
        if (rule.material.has_value() && !definedMaterials.contains(*rule.material)) {
            diags.error(codes::kErrUndefinedMaterialRef,
                        std::format("rule references undefined material '{}'", *rule.material));
        }

        // max_segments_circle must be positive.
        if (rule.tessellation.has_value() && rule.tessellation->maxSegmentsCircle.has_value() &&
            *rule.tessellation->maxSegmentsCircle <= 0) {
            diags.error(codes::kErrNegativeTolerance,
                        std::format("rule max_segments_circle must be > 0, got {}",
                                    *rule.tessellation->maxSegmentsCircle));
        }

        // A rule with material set but no match predicate matches every node.
        // Combined with material's last-match-wins resolution, it silently
        // shadows every earlier `material` rule (e.g. those merged from
        // included files). This is almost always unintended.
        if (rule.material.has_value() && !rule.match.has_value() && sawEarlierMaterialRule) {
            diags.warn(codes::kWarnConfigUnconditionalMaterialRule,
                       std::format("rule sets material = '{}' with no match predicate; "
                                   "it will shadow every earlier material rule "
                                   "(material resolution is last-match-wins). "
                                   "Add a `match` predicate to scope it.",
                                   *rule.material));
        }
        if (rule.material.has_value()) {
            sawEarlierMaterialRule = true;
        }
    }

    // drop_coincident_faces operates on a merge_descendants group, so it is a
    // silent no-op unless merge_descendants is enabled for the same nodes.
    // merge_descendants can come from a *different* rule or from defaults
    // (last-match-wins per field), so we can't tie the two to a single rule
    // without the geometry. The soundly-detectable mistake is enabling
    // drop_coincident_faces while merge_descendants is enabled *nowhere* — then
    // it can never take effect. (Disjoint-node cases are reported at build time,
    // where the drop pass simply removes 0 faces.)
    bool dropEnabledSomewhere = cfg.tessellationDefaults.dropCoincidentFaces.value_or(false);
    bool mergeEnabledSomewhere = cfg.tessellationDefaults.mergeDescendants.value_or(false);
    for (const auto &rule : cfg.rules) {
        if (!rule.tessellation.has_value()) {
            continue;
        }
        dropEnabledSomewhere |= rule.tessellation->dropCoincidentFaces.value_or(false);
        mergeEnabledSomewhere |= rule.tessellation->mergeDescendants.value_or(false);
    }
    if (dropEnabledSomewhere && !mergeEnabledSomewhere) {
        diags.warn(codes::kWarnConfigDropWithoutMerge,
                   "drop_coincident_faces is enabled but merge_descendants is never enabled; "
                   "drop_coincident_faces operates on a merge_descendants group and has no "
                   "effect without it. Set merge_descendants = true on the same nodes.");
    }

    return diags;
}

} // namespace nodehammer::config

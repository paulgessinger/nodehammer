#include <config/config_validator.hpp>
#include <config/predicate_parser.hpp>
#include <diagnostic_codes.hpp>

#include <cmath>
#include <format>
#include <unordered_set>

namespace nodehammer::config {

DiagnosticList ConfigValidator::validateWireSettings(const Rule::Tessellation &tessellation) {
    DiagnosticList diags;
    if (tessellation.wireCut && *tessellation.wireCut != "exact" &&
        *tessellation.wireCut != "centerline")
        diags.error(codes::kErrConfigParse, "wire_cut must be 'exact' or 'centerline'");
    if (tessellation.wireLodPixels &&
        (!std::isfinite(*tessellation.wireLodPixels) || *tessellation.wireLodPixels < 0))
        diags.error(codes::kErrConfigParse, "wire_lod_pixels must be finite and nonnegative");
    if (tessellation.wireLodDistance &&
        (!std::isfinite(*tessellation.wireLodDistance) || *tessellation.wireLodDistance < 0))
        diags.error(codes::kErrConfigParse, "wire_lod_distance must be finite and nonnegative");
    if (tessellation.wireLodHintPixels &&
        (!std::isfinite(*tessellation.wireLodHintPixels) || *tessellation.wireLodHintPixels < 0))
        diags.error(codes::kErrConfigParse, "wire_lod_hint_pixels must be finite and nonnegative");
    if (tessellation.wireLodDistanceFactors) {
        const auto &v = *tessellation.wireLodDistanceFactors;
        bool valid = v.size() == 3;
        double previous = 0;
        for (double x : v) {
            valid &= std::isfinite(x) && x > previous;
            previous = x;
        }
        if (!valid)
            diags.error(codes::kErrConfigParse, "wire_lod_distance_factors must contain three "
                                                "finite positive strictly increasing values");
    }
    if (tessellation.wireLodFractions) {
        const auto &v = *tessellation.wireLodFractions;
        bool valid = v.size() == 4;
        double previous = 1;
        for (double x : v) {
            valid &= std::isfinite(x) && x >= 0 && x <= previous;
            previous = x;
        }
        if (!valid)
            diags.error(
                codes::kErrConfigParse,
                "wire_lod_fractions must contain four nonincreasing finite fractions in [0, 1]");
    }
    return diags;
}

DiagnosticList ConfigValidator::validateDaughterSettings(const Rule::Tessellation &tessellation) {
    DiagnosticList diags;
    if (tessellation.subtractDaughtersMatch && !tessellation.subtractDaughtersMatch->empty()) {
        const auto parsed = parsePredicateExpr(*tessellation.subtractDaughtersMatch);
        if (!parsed)
            diags.error(
                codes::kErrConfigParse,
                std::format("invalid subtract_daughters_match predicate: {}", parsed.error()));
    }
    return diags;
}

DiagnosticList ConfigValidator::validateGeometry(const Rule::Geometry &geometry) {
    DiagnosticList diags;
    auto error = [&](std::string message) {
        diags.error(codes::kErrConfigParse, std::move(message), "rules.geometry");
    };
    if (geometry.replaceDescendants.empty() && !geometry.driftChamber)
        error("geometry must specify replace_descendants or drift_chamber");
    if (!geometry.replaceDescendants.empty() && geometry.replaceDescendants != "convex_hull")
        error("replace_descendants must be the string 'convex_hull'");
    if (!geometry.replaceDescendants.empty() && geometry.driftChamber)
        error("replace_descendants and drift_chamber cannot be combined in one rule");
    if (geometry.driftChamber) {
        const auto &drift = *geometry.driftChamber;
        if (drift.detect != "auto")
            error("drift_chamber.detect must be 'auto'");
        if (drift.gasRepresentation != "parent")
            error("drift_chamber.gas_representation must be 'parent'");
        if (drift.wireRepresentation != "instanced")
            error("drift_chamber.wire_representation must be 'instanced'");
        for (const auto &[key, value] :
             {std::pair{"gas", drift.gas}, std::pair{"wires", drift.wires}}) {
            if (value) {
                const auto parsed = parsePredicateExpr(*value);
                if (!parsed)
                    error(
                        std::format("invalid drift_chamber.{} predicate: {}", key, parsed.error()));
            }
        }
    }
    return diags;
}

DiagnosticList ConfigValidator::validate(const NHConfig &cfg) {
    DiagnosticList diags;

    auto validateTrapTolerance = [&](const Rule::Tessellation &t) {
        if (t.smoothAngle &&
            (!std::isfinite(*t.smoothAngle) || *t.smoothAngle < 0 || *t.smoothAngle > 180))
            diags.error(codes::kErrConfigParse,
                        "smooth_angle must be finite and between 0 and 180 degrees");
        diags.append(validateWireSettings(t));
        diags.append(validateDaughterSettings(t));
        if (t.genericTrapTolerance &&
            (!std::isfinite(*t.genericTrapTolerance) || *t.genericTrapTolerance <= 0))
            diags.error(codes::kErrNegativeTolerance,
                        "generic_trap_tolerance must be finite and positive");
    };
    validateTrapTolerance(cfg.tessellationDefaults);
    for (const auto &rule : cfg.rules)
        if (rule.tessellation)
            validateTrapTolerance(*rule.tessellation);
    // Build set of defined material names for reference checking.
    std::unordered_set<std::string> definedMaterials;
    for (const auto &mat : cfg.materials) {
        definedMaterials.insert(mat.name);
    }

    bool sawEarlierMaterialRule = false;
    for (const auto &rule : cfg.rules) {
        if (rule.geometry)
            diags.append(validateGeometry(*rule.geometry));
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

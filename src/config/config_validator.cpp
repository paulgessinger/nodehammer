#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <format>
#include <unordered_set>

namespace nodehammer {

DiagnosticList ConfigValidator::validate(const NHConfig &cfg) {
    DiagnosticList diags;

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

    return diags;
}

} // namespace nodehammer

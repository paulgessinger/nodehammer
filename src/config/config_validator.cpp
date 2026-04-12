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
    }

    return diags;
}

} // namespace nodehammer

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

    // material_rules: every referenced material must be defined.
    for (const auto &rule : cfg.materialRules) {
        if (!definedMaterials.contains(rule.materialName)) {
            diags.error(
                std::string{codes::kErrUndefinedMaterialRef},
                std::format("material_rule references undefined material '{}'", rule.materialName),
                rule.nameGlob);
        }
    }

    // tessellation_rules: max_segments_circle must be positive.
    for (const auto &rule : cfg.tessellationRules) {
        if (rule.maxSegmentsCircle <= 0) {
            diags.error(std::string{codes::kErrNegativeTolerance},
                        std::format("tessellation_rule max_segments_circle must be > 0, got {}",
                                    rule.maxSegmentsCircle),
                        rule.nameGlob);
        }
    }

    // output: path must be set.
    if (cfg.exportCfg.outputPath.empty()) {
        diags.error(std::string{codes::kErrMissingOutputPath},
                    "output.path is required but not set");
    }

    return diags;
}

} // namespace nodehammer

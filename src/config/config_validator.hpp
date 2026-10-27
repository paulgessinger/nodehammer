#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>

namespace nodehammer::config {

struct ConfigValidator {
    /// Validates cross-references and constraints within an already-parsed NHConfig.
    /// Returns diagnostics found. Empty list = valid.
    [[nodiscard]] static DiagnosticList
    validateWireSettings(const Rule::Tessellation &tessellation);
    [[nodiscard]] static DiagnosticList
    validateDaughterSettings(const Rule::Tessellation &tessellation);
    [[nodiscard]] static DiagnosticList validateGeometry(const Rule::Geometry &geometry);
    [[nodiscard]] static DiagnosticList validate(const NHConfig &config);
};

} // namespace nodehammer::config

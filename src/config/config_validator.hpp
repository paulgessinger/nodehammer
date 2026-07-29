#pragma once

#include <config/config_ast.hpp>
#include <ir/diagnostics.hpp>

namespace nodehammer {

struct ConfigValidator {
    /// Validates cross-references and constraints within an already-parsed NHConfig.
    /// Returns diagnostics found. Empty list = valid.
    [[nodiscard]] static DiagnosticList validate(const NHConfig &config);
};

} // namespace nodehammer

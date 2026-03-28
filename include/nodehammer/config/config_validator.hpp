#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>

namespace nodehammer {

struct ConfigValidator {
    /// Validates cross-references and constraints within an already-parsed NHConfig.
    /// Returns diagnostics found. Empty list = valid.
    [[nodiscard]] static DiagnosticList validate(const NHConfig &config);
};

} // namespace nodehammer

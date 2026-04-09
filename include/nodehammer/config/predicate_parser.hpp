#pragma once

#include <nodehammer/config/config_ast.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace nodehammer {

/// Parse a predicate expression string into a PredicateExpr AST node.
/// Returns the parsed expression or an error message.
[[nodiscard]] std::expected<PredicateExpr, std::string> parsePredicateExpr(std::string_view input);

} // namespace nodehammer

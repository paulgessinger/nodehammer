#pragma once

#include <nodehammer/config/config_ast.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

/// Parse a predicate expression string into a PredicateExpr AST node.
/// Returns the parsed expression or an error message.
/// ```
/// expr       ← or_expr
/// or_expr    ← and_expr ('||' and_expr)*
/// and_expr   ← unary_expr ('&&' unary_expr)*
/// unary_expr ← '!' unary_expr / primary
/// primary    ← func_call / atom / '(' expr ')'
///
/// func_call  ← ('any' / 'all') '(' expr (',' expr)* ','? ')'
///
/// atom       ← 'true' / 'false' / 'is_leaf' / tag_expr / path_expr / name_expr
/// tag_expr   ← 'tag.' IDENT ('==' STRING)?
/// path_expr  ← 'path' '~=' STRING
/// name_expr  ← 'name' '~=' STRING
///
/// STRING     ← '"' [^"]* '"'
/// IDENT      ← [a-zA-Z_][a-zA-Z0-9_]*
/// ```
[[nodiscard]] std::expected<PredicateExpr, std::string> parsePredicateExpr(std::string_view input);

/// Combine predicates under OR, simplifying the degenerate cases: an empty list
/// collapses to the OR identity (`false`), a single predicate is returned bare
/// (no wrapper), and two or more become an `OrPredicate`. Shared by the
/// expression parser and the config front-ends (TOML + Lua) so scalar-or-list
/// fields (`match`, `keep_if`, …) assemble identically everywhere.
[[nodiscard]] PredicateExpr combineOr(std::vector<PredicateExpr> operands);

/// Combine predicates under AND, simplifying: empty → the AND identity (`true`),
/// single → the predicate bare, else an `AndPredicate`.
[[nodiscard]] PredicateExpr combineAnd(std::vector<PredicateExpr> operands);

} // namespace nodehammer

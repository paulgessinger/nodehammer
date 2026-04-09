#pragma once

#include <nodehammer/config/config_ast.hpp>

#include <expected>
#include <string>
#include <string_view>

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

} // namespace nodehammer

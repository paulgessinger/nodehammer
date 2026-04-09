#include <nodehammer/config/predicate_parser.hpp>

#include <cctype>
#include <format>
#include <vector>

namespace nodehammer {

namespace {

// ── Token types ──────────────────────────────────────────────────────────────

enum class TokenType {
    // Literals / identifiers
    String, // "..."
    Ident,  // name, path, tag, is_leaf, true, false, any, all
    // Operators
    And,     // &&
    Or,      // ||
    Not,     // !
    TildeEq, // ~=
    EqEq,    // ==
    Dot,     // .
    // Delimiters
    LParen, // (
    RParen, // )
    Comma,  // ,
    // End
    Eof,
};

struct Token {
    TokenType type;
    std::string_view text;
    std::size_t pos; // byte offset in the input
};

// ── Lexer ────────────────────────────────────────────────────────────────────

class Lexer {
  public:
    explicit Lexer(std::string_view input) : input_(input) {}

    std::expected<std::vector<Token>, std::string> tokenize() {
        std::vector<Token> tokens;
        while (pos_ < input_.size()) {
            skipWhitespace();
            if (pos_ >= input_.size()) {
                break;
            }

            const char c = input_[pos_];

            if (c == '"') {
                auto tok = lexString();
                if (!tok) {
                    return std::unexpected(std::move(tok.error()));
                }
                tokens.push_back(*tok);
            } else if (c == '&') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '&') {
                    tokens.push_back({TokenType::And, input_.substr(pos_, 2), pos_});
                    pos_ += 2;
                } else {
                    return std::unexpected(
                        std::format("unexpected '&' at position {}; did you mean '&&'?", pos_));
                }
            } else if (c == '|') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '|') {
                    tokens.push_back({TokenType::Or, input_.substr(pos_, 2), pos_});
                    pos_ += 2;
                } else {
                    return std::unexpected(
                        std::format("unexpected '|' at position {}; did you mean '||'?", pos_));
                }
            } else if (c == '!') {
                tokens.push_back({TokenType::Not, input_.substr(pos_, 1), pos_});
                pos_ += 1;
            } else if (c == '~') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    tokens.push_back({TokenType::TildeEq, input_.substr(pos_, 2), pos_});
                    pos_ += 2;
                } else {
                    return std::unexpected(
                        std::format("unexpected '~' at position {}; did you mean '~='?", pos_));
                }
            } else if (c == '=') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    tokens.push_back({TokenType::EqEq, input_.substr(pos_, 2), pos_});
                    pos_ += 2;
                } else {
                    return std::unexpected(
                        std::format("unexpected '=' at position {}; did you mean '=='?", pos_));
                }
            } else if (c == '.') {
                tokens.push_back({TokenType::Dot, input_.substr(pos_, 1), pos_});
                pos_ += 1;
            } else if (c == '(') {
                tokens.push_back({TokenType::LParen, input_.substr(pos_, 1), pos_});
                pos_ += 1;
            } else if (c == ')') {
                tokens.push_back({TokenType::RParen, input_.substr(pos_, 1), pos_});
                pos_ += 1;
            } else if (c == ',') {
                tokens.push_back({TokenType::Comma, input_.substr(pos_, 1), pos_});
                pos_ += 1;
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                tokens.push_back(lexIdent());
            } else {
                return std::unexpected(
                    std::format("unexpected character '{}' at position {}", c, pos_));
            }
        }
        tokens.push_back({TokenType::Eof, {}, pos_});
        return tokens;
    }

  private:
    void skipWhitespace() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    std::expected<Token, std::string> lexString() {
        const std::size_t start = pos_;
        ++pos_; // skip opening "
        while (pos_ < input_.size() && input_[pos_] != '"') {
            if (input_[pos_] == '\\' && pos_ + 1 < input_.size()) {
                pos_ += 2; // skip escape sequence
            } else {
                ++pos_;
            }
        }
        if (pos_ >= input_.size()) {
            return std::unexpected(
                std::format("unterminated string starting at position {}", start));
        }
        ++pos_; // skip closing "
        // text includes the quotes; the parser will strip them.
        return Token{TokenType::String, input_.substr(start, pos_ - start), start};
    }

    Token lexIdent() {
        const std::size_t start = pos_;
        while (pos_ < input_.size() &&
               (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_')) {
            ++pos_;
        }
        return {TokenType::Ident, input_.substr(start, pos_ - start), start};
    }

    std::string_view input_;
    std::size_t pos_{0};
};

// ── Parser ───────────────────────────────────────────────────────────────────

class Parser {
  public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::expected<PredicateExpr, std::string> parse() {
        auto result = parseOrExpr();
        if (!result) {
            return result;
        }
        if (peek().type != TokenType::Eof) {
            return std::unexpected(
                std::format("unexpected token '{}' at position {}", peek().text, peek().pos));
        }
        return result;
    }

  private:
    // or_expr ← and_expr ('||' and_expr)*
    std::expected<PredicateExpr, std::string> parseOrExpr() {
        auto left = parseAndExpr();
        if (!left) {
            return left;
        }
        std::vector<PredicateExpr> operands;
        operands.push_back(std::move(*left));
        while (peek().type == TokenType::Or) {
            advance(); // consume ||
            auto right = parseAndExpr();
            if (!right) {
                return right;
            }
            operands.push_back(std::move(*right));
        }
        if (operands.size() == 1) {
            return std::move(operands[0]);
        }
        return PredicateExpr{std::make_shared<OrPredicate>(OrPredicate{std::move(operands)})};
    }

    // and_expr ← unary_expr ('&&' unary_expr)*
    std::expected<PredicateExpr, std::string> parseAndExpr() {
        auto left = parseUnaryExpr();
        if (!left) {
            return left;
        }
        std::vector<PredicateExpr> operands;
        operands.push_back(std::move(*left));
        while (peek().type == TokenType::And) {
            advance(); // consume &&
            auto right = parseUnaryExpr();
            if (!right) {
                return right;
            }
            operands.push_back(std::move(*right));
        }
        if (operands.size() == 1) {
            return std::move(operands[0]);
        }
        return PredicateExpr{std::make_shared<AndPredicate>(AndPredicate{std::move(operands)})};
    }

    // unary_expr ← '!' unary_expr / primary
    std::expected<PredicateExpr, std::string> parseUnaryExpr() {
        if (peek().type == TokenType::Not) {
            advance(); // consume !
            auto operand = parseUnaryExpr();
            if (!operand) {
                return operand;
            }
            return PredicateExpr{std::make_shared<NotPredicate>(NotPredicate{std::move(*operand)})};
        }
        return parsePrimary();
    }

    // primary ← func_call / atom / '(' expr ')'
    std::expected<PredicateExpr, std::string> parsePrimary() {
        const auto &tok = peek();

        // Parenthesized expression
        if (tok.type == TokenType::LParen) {
            advance(); // consume (
            auto expr = parseOrExpr();
            if (!expr) {
                return expr;
            }
            if (peek().type != TokenType::RParen) {
                return std::unexpected(
                    std::format("expected ')' at position {}, got '{}'", peek().pos, peek().text));
            }
            advance(); // consume )
            return expr;
        }

        if (tok.type == TokenType::Ident) {
            // Function call: any(...) / all(...)
            if ((tok.text == "any" || tok.text == "all") && pos_ + 1 < tokens_.size() &&
                tokens_[pos_ + 1].type == TokenType::LParen) {
                return parseFuncCall();
            }

            // Atoms
            if (tok.text == "true") {
                advance();
                return PredicateExpr{BoolPredicate{true}};
            }
            if (tok.text == "false") {
                advance();
                return PredicateExpr{BoolPredicate{false}};
            }
            if (tok.text == "is_leaf") {
                advance();
                return PredicateExpr{IsLeafPredicate{}};
            }

            // tag.KEY or tag.KEY == "value"
            if (tok.text == "tag") {
                return parseTagExpr();
            }

            // path ~= "pattern"
            if (tok.text == "path") {
                return parseGlobExpr<PathGlobPredicate>("path");
            }

            // name ~= "pattern"
            if (tok.text == "name") {
                return parseGlobExpr<NameGlobPredicate>("name");
            }

            return std::unexpected(
                std::format("unknown identifier '{}' at position {}", tok.text, tok.pos));
        }

        if (tok.type == TokenType::Eof) {
            return std::unexpected("unexpected end of expression");
        }

        return std::unexpected(
            std::format("unexpected token '{}' at position {}", tok.text, tok.pos));
    }

    // func_call ← ('any' / 'all') '(' expr (',' expr)* ')'
    std::expected<PredicateExpr, std::string> parseFuncCall() {
        const auto funcName = peek().text;
        advance(); // consume function name
        if (peek().type != TokenType::LParen) {
            return std::unexpected(
                std::format("expected '(' after '{}' at position {}", funcName, peek().pos));
        }
        advance(); // consume (

        std::vector<PredicateExpr> args;
        if (peek().type != TokenType::RParen) {
            auto arg = parseOrExpr();
            if (!arg) {
                return arg;
            }
            args.push_back(std::move(*arg));
            while (peek().type == TokenType::Comma) {
                advance(); // consume ,
                // Allow trailing comma: if next token is ')', stop.
                if (peek().type == TokenType::RParen) {
                    break;
                }
                auto nextArg = parseOrExpr();
                if (!nextArg) {
                    return nextArg;
                }
                args.push_back(std::move(*nextArg));
            }
        }
        if (peek().type != TokenType::RParen) {
            return std::unexpected(
                std::format("expected ')' at position {}, got '{}'", peek().pos, peek().text));
        }
        advance(); // consume )

        if (funcName == "any") {
            return PredicateExpr{std::make_shared<OrPredicate>(OrPredicate{std::move(args)})};
        }
        return PredicateExpr{std::make_shared<AndPredicate>(AndPredicate{std::move(args)})};
    }

    // tag_expr ← 'tag' '.' IDENT ('==' STRING)?
    std::expected<PredicateExpr, std::string> parseTagExpr() {
        advance(); // consume 'tag'
        if (peek().type != TokenType::Dot) {
            return std::unexpected(
                std::format("expected '.' after 'tag' at position {}", peek().pos));
        }
        advance(); // consume .
        if (peek().type != TokenType::Ident) {
            return std::unexpected(
                std::format("expected tag key identifier at position {}", peek().pos));
        }
        std::string key{peek().text};
        advance(); // consume key

        std::optional<std::string> value;
        if (peek().type == TokenType::EqEq) {
            advance(); // consume ==
            if (peek().type != TokenType::String) {
                return std::unexpected(
                    std::format("expected string after '==' at position {}", peek().pos));
            }
            value = stripQuotes(peek().text);
            advance(); // consume string
        }
        return PredicateExpr{TagPredicate{std::move(key), std::move(value)}};
    }

    // glob_expr ← ('path' / 'name') '~=' STRING
    template <typename GlobType>
    std::expected<PredicateExpr, std::string> parseGlobExpr(std::string_view keyword) {
        advance(); // consume keyword
        if (peek().type != TokenType::TildeEq) {
            return std::unexpected(
                std::format("expected '~=' after '{}' at position {}", keyword, peek().pos));
        }
        advance(); // consume ~=
        if (peek().type != TokenType::String) {
            return std::unexpected(
                std::format("expected string pattern after '~=' at position {}", peek().pos));
        }
        std::string pattern{stripQuotes(peek().text)};
        advance(); // consume string
        return PredicateExpr{GlobType{std::move(pattern)}};
    }

    static std::string stripQuotes(std::string_view s) {
        // Remove surrounding quotes: "..." → ...
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            return std::string{s.substr(1, s.size() - 2)};
        }
        return std::string{s};
    }

    const Token &peek() const { return tokens_[pos_]; }
    void advance() { ++pos_; }

    std::vector<Token> tokens_;
    std::size_t pos_{0};
};

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

std::expected<PredicateExpr, std::string> parsePredicateExpr(std::string_view input) {
    Lexer lexer{input};
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return std::unexpected(std::move(tokens.error()));
    }
    Parser parser{std::move(*tokens)};
    return parser.parse();
}

} // namespace nodehammer

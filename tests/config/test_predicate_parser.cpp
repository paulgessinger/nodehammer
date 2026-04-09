#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/predicate_parser.hpp>

using namespace nodehammer;

// ── Helper: assert parse succeeds and return the expression ──────────────────

static PredicateExpr mustParse(std::string_view input) {
    auto result = parsePredicateExpr(input);
    REQUIRE(result.has_value());
    return std::move(*result);
}

static void mustFail(std::string_view input) {
    auto result = parsePredicateExpr(input);
    REQUIRE_FALSE(result.has_value());
}

// ── Atoms ────────────────────────────────────────────────────────────────────

TEST_CASE("PredicateParser: true literal", "[config][parser]") {
    auto expr = mustParse("true");
    REQUIRE(std::holds_alternative<BoolPredicate>(expr.data));
    REQUIRE(std::get<BoolPredicate>(expr.data).value == true);
}

TEST_CASE("PredicateParser: false literal", "[config][parser]") {
    auto expr = mustParse("false");
    REQUIRE(std::holds_alternative<BoolPredicate>(expr.data));
    REQUIRE(std::get<BoolPredicate>(expr.data).value == false);
}

TEST_CASE("PredicateParser: is_leaf", "[config][parser]") {
    auto expr = mustParse("is_leaf");
    REQUIRE(std::holds_alternative<IsLeafPredicate>(expr.data));
}

TEST_CASE("PredicateParser: name glob", "[config][parser]") {
    auto expr = mustParse(R"(name ~= "sensor*")");
    REQUIRE(std::holds_alternative<NameGlobPredicate>(expr.data));
    REQUIRE(std::get<NameGlobPredicate>(expr.data).pattern == "sensor*");
}

TEST_CASE("PredicateParser: path glob", "[config][parser]") {
    auto expr = mustParse(R"(path ~= "**/Pixels/**")");
    REQUIRE(std::holds_alternative<PathGlobPredicate>(expr.data));
    REQUIRE(std::get<PathGlobPredicate>(expr.data).pattern == "**/Pixels/**");
}

TEST_CASE("PredicateParser: tag key exists", "[config][parser]") {
    auto expr = mustParse("tag.sensitive");
    REQUIRE(std::holds_alternative<TagPredicate>(expr.data));
    const auto &tag = std::get<TagPredicate>(expr.data);
    REQUIRE(tag.key == "sensitive");
    REQUIRE_FALSE(tag.value.has_value());
}

TEST_CASE("PredicateParser: tag key == value", "[config][parser]") {
    auto expr = mustParse(R"(tag.sensitive == "true")");
    REQUIRE(std::holds_alternative<TagPredicate>(expr.data));
    const auto &tag = std::get<TagPredicate>(expr.data);
    REQUIRE(tag.key == "sensitive");
    REQUIRE(tag.value == "true");
}

TEST_CASE("PredicateParser: tag with underscore key", "[config][parser]") {
    auto expr = mustParse(R"(tag.sub_detector == "barrel")");
    const auto &tag = std::get<TagPredicate>(expr.data);
    REQUIRE(tag.key == "sub_detector");
    REQUIRE(tag.value == "barrel");
}

// ── Logical operators ────────────────────────────────────────────────────────

TEST_CASE("PredicateParser: NOT", "[config][parser]") {
    auto expr = mustParse("!is_leaf");
    REQUIRE(std::holds_alternative<std::shared_ptr<NotPredicate>>(expr.data));
    const auto &inner = std::get<std::shared_ptr<NotPredicate>>(expr.data)->operand;
    REQUIRE(std::holds_alternative<IsLeafPredicate>(inner.data));
}

TEST_CASE("PredicateParser: double NOT", "[config][parser]") {
    auto expr = mustParse("!!true");
    REQUIRE(std::holds_alternative<std::shared_ptr<NotPredicate>>(expr.data));
    const auto &inner = std::get<std::shared_ptr<NotPredicate>>(expr.data)->operand;
    REQUIRE(std::holds_alternative<std::shared_ptr<NotPredicate>>(inner.data));
}

TEST_CASE("PredicateParser: AND", "[config][parser]") {
    auto expr = mustParse(R"(tag.sensitive == "true" && is_leaf)");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 2);
    REQUIRE(std::holds_alternative<TagPredicate>(ops[0].data));
    REQUIRE(std::holds_alternative<IsLeafPredicate>(ops[1].data));
}

TEST_CASE("PredicateParser: OR", "[config][parser]") {
    auto expr = mustParse(R"(path ~= "**/A/**" || path ~= "**/B/**")");
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<OrPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 2);
}

TEST_CASE("PredicateParser: chained AND", "[config][parser]") {
    auto expr = mustParse("true && false && is_leaf");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 3);
}

TEST_CASE("PredicateParser: chained OR", "[config][parser]") {
    auto expr = mustParse("true || false || is_leaf");
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<OrPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 3);
}

// ── Precedence ───────────────────────────────────────────────────────────────

TEST_CASE("PredicateParser: AND binds tighter than OR", "[config][parser]") {
    // "a || b && c" should parse as "a || (b && c)"
    auto expr = mustParse("true || false && is_leaf");
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(expr.data));
    const auto &orOps = std::get<std::shared_ptr<OrPredicate>>(expr.data)->operands;
    REQUIRE(orOps.size() == 2);
    REQUIRE(std::holds_alternative<BoolPredicate>(orOps[0].data)); // true
    REQUIRE(
        std::holds_alternative<std::shared_ptr<AndPredicate>>(orOps[1].data)); // false && is_leaf
}

TEST_CASE("PredicateParser: NOT binds tighter than AND", "[config][parser]") {
    // "!true && false" should parse as "(!true) && false"
    auto expr = mustParse("!true && false");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &andOps = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(andOps.size() == 2);
    REQUIRE(std::holds_alternative<std::shared_ptr<NotPredicate>>(andOps[0].data));
    REQUIRE(std::holds_alternative<BoolPredicate>(andOps[1].data));
}

// ── Parentheses ──────────────────────────────────────────────────────────────

TEST_CASE("PredicateParser: parentheses override precedence", "[config][parser]") {
    // "(a || b) && c" → AND with OR as first operand
    auto expr = mustParse("(true || false) && is_leaf");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &andOps = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(andOps.size() == 2);
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(andOps[0].data));
    REQUIRE(std::holds_alternative<IsLeafPredicate>(andOps[1].data));
}

TEST_CASE("PredicateParser: nested parentheses", "[config][parser]") {
    auto expr = mustParse("((true))");
    REQUIRE(std::holds_alternative<BoolPredicate>(expr.data));
}

// ── Function calls ───────────────────────────────────────────────────────────

TEST_CASE("PredicateParser: any() produces OrPredicate", "[config][parser]") {
    auto expr = mustParse(R"(any(path ~= "**/A/**", path ~= "**/B/**", path ~= "**/C/**"))");
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<OrPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 3);
}

TEST_CASE("PredicateParser: all() produces AndPredicate", "[config][parser]") {
    auto expr = mustParse(R"(all(tag.sensitive == "true", is_leaf))");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 2);
}

TEST_CASE("PredicateParser: any() with single argument", "[config][parser]") {
    auto expr = mustParse("any(true)");
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<OrPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 1);
}

TEST_CASE("PredicateParser: nested function calls", "[config][parser]") {
    auto expr =
        mustParse(R"(all(tag.sensitive == "true", any(path ~= "**/A/**", path ~= "**/B/**")))");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 2);
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(ops[1].data));
}

// ── Compound expressions (realistic) ────────────────────────────────────────

TEST_CASE("PredicateParser: realistic sensor selection expression", "[config][parser]") {
    auto expr = mustParse(
        R"(tag.sensitive == "true" && any(path ~= "**/Pixels/**", path ~= "**/ShortStrips/**", path ~= "**/LongStrips/**"))");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &andOps = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(andOps.size() == 2);
    REQUIRE(std::holds_alternative<TagPredicate>(andOps[0].data));
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(andOps[1].data));
    REQUIRE(std::get<std::shared_ptr<OrPredicate>>(andOps[1].data)->operands.size() == 3);
}

TEST_CASE("PredicateParser: mixed operators and functions", "[config][parser]") {
    auto expr = mustParse(R"(!is_leaf && (tag.x == "a" || tag.y == "b") && name ~= "sensor*")");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
    const auto &ops = std::get<std::shared_ptr<AndPredicate>>(expr.data)->operands;
    REQUIRE(ops.size() == 3);
    REQUIRE(std::holds_alternative<std::shared_ptr<NotPredicate>>(ops[0].data));
    REQUIRE(std::holds_alternative<std::shared_ptr<OrPredicate>>(ops[1].data));
    REQUIRE(std::holds_alternative<NameGlobPredicate>(ops[2].data));
}

// ── Whitespace handling ──────────────────────────────────────────────────────

TEST_CASE("PredicateParser: no whitespace", "[config][parser]") {
    auto expr = mustParse(R"(tag.x=="a"&&is_leaf)");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
}

TEST_CASE("PredicateParser: extra whitespace", "[config][parser]") {
    auto expr = mustParse(R"(  tag.x  ==  "a"  &&  is_leaf  )");
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(expr.data));
}

// ── Error cases ──────────────────────────────────────────────────────────────

TEST_CASE("PredicateParser: empty input → error", "[config][parser]") { mustFail(""); }

TEST_CASE("PredicateParser: unterminated string → error", "[config][parser]") {
    mustFail(R"(name ~= "unterminated)");
}

TEST_CASE("PredicateParser: missing pattern after ~= → error", "[config][parser]") {
    mustFail("name ~=");
}

TEST_CASE("PredicateParser: missing tag key → error", "[config][parser]") { mustFail("tag."); }

TEST_CASE("PredicateParser: missing dot after tag → error", "[config][parser]") {
    mustFail("tag == \"x\"");
}

TEST_CASE("PredicateParser: unknown identifier → error", "[config][parser]") { mustFail("foobar"); }

TEST_CASE("PredicateParser: unclosed paren → error", "[config][parser]") {
    mustFail("(true && false");
}

TEST_CASE("PredicateParser: missing operand after && → error", "[config][parser]") {
    mustFail("true &&");
}

TEST_CASE("PredicateParser: missing operand after || → error", "[config][parser]") {
    mustFail("true ||");
}

TEST_CASE("PredicateParser: missing operand after ! → error", "[config][parser]") { mustFail("!"); }

TEST_CASE("PredicateParser: trailing token → error", "[config][parser]") { mustFail("true false"); }

TEST_CASE("PredicateParser: single & → error with hint", "[config][parser]") {
    auto result = parsePredicateExpr("true & false");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().find("&&") != std::string::npos);
}

TEST_CASE("PredicateParser: single | → error with hint", "[config][parser]") {
    auto result = parsePredicateExpr("true | false");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().find("||") != std::string::npos);
}

TEST_CASE("PredicateParser: any() missing closing paren → error", "[config][parser]") {
    mustFail("any(true, false");
}

TEST_CASE("PredicateParser: tag.key == non-string → error", "[config][parser]") {
    mustFail("tag.x == 42");
}

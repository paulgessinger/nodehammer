#include <catch2/catch_test_macros.hpp>
#include <config/config_ast.hpp>
#include <selection/predicate.hpp>

#include <map>

using namespace nodehammer;
using namespace nodehammer::config;
using namespace nodehammer::selection;

// ── matchGlob ─────────────────────────────────────────────────────────────────

TEST_CASE("matchGlob: exact literal match", "[selection][predicate]") {
    REQUIRE(matchGlob("foo", "foo"));
    REQUIRE_FALSE(matchGlob("foo", "bar"));
    REQUIRE_FALSE(matchGlob("foo", "foo/bar"));
}

TEST_CASE("matchGlob: empty pattern matches only empty text", "[selection][predicate]") {
    REQUIRE(matchGlob("", ""));
    REQUIRE_FALSE(matchGlob("", "a"));
}

TEST_CASE("matchGlob: * matches within a single segment", "[selection][predicate]") {
    REQUIRE(matchGlob("*", "foo"));
    REQUIRE(matchGlob("*", ""));
    REQUIRE(matchGlob("Tracker*", "TrackerModule"));
    REQUIRE(matchGlob("*Module", "TrackerModule"));
    REQUIRE(matchGlob("*Track*", "MyTracker"));
}

TEST_CASE("matchGlob: * does NOT cross /", "[selection][predicate]") {
    REQUIRE_FALSE(matchGlob("*", "foo/bar"));
    REQUIRE_FALSE(matchGlob("*Track*", "foo/Tracker"));
    REQUIRE_FALSE(matchGlob("foo/*", "foo/bar/baz"));
}

TEST_CASE("matchGlob: ** matches across /", "[selection][predicate]") {
    REQUIRE(matchGlob("**", "foo/bar/baz"));
    REQUIRE(matchGlob("**", ""));
    REQUIRE(matchGlob("/world/**", "/world/tracker/module"));
    REQUIRE_FALSE(matchGlob("/world/**", "/other/tracker"));
}

TEST_CASE("matchGlob: **/foo matches at any depth", "[selection][predicate]") {
    REQUIRE(matchGlob("**/sensor", "sensor"));
    REQUIRE(matchGlob("**/sensor", "/sensor"));
    REQUIRE(matchGlob("**/sensor", "a/b/sensor"));
    REQUIRE_FALSE(matchGlob("**/sensor", "a/b/sensor_v2"));
}

TEST_CASE("matchGlob: /world/*/child stops at one segment", "[selection][predicate]") {
    REQUIRE(matchGlob("/world/*/child", "/world/inner/child"));
    REQUIRE_FALSE(matchGlob("/world/*/child", "/world/a/b/child"));
}

// ── Factory functions ─────────────────────────────────────────────────────────

static std::map<std::string, std::string> noTags;

TEST_CASE("makeNameGlobPredicate: matches node name", "[selection][predicate]") {
    auto p = makeNameGlobPredicate("Tracker*");
    NodeView v;
    v.tags = &noTags;

    v.name = "TrackerModule";
    REQUIRE(p(v));

    v.name = "CalModule";
    REQUIRE_FALSE(p(v));
}

TEST_CASE("makePathGlobPredicate: ** matches across segments", "[selection][predicate]") {
    auto p = makePathGlobPredicate("/world/**/sensor");
    NodeView v;
    v.tags = &noTags;

    v.path = "/world/tracker/layer0/sensor";
    REQUIRE(p(v));

    v.path = "/world/sensor";
    REQUIRE(p(v));

    v.path = "/other/tracker/sensor";
    REQUIRE_FALSE(p(v));
}

TEST_CASE("makeTagPredicate: key-only match", "[selection][predicate]") {
    auto p = makeTagPredicate("sensitive", std::nullopt);
    std::map<std::string, std::string> tags{{"sensitive", "true"}};
    std::map<std::string, std::string> emptyTags;

    NodeView v;
    v.tags = &tags;
    REQUIRE(p(v));

    v.tags = &emptyTags;
    REQUIRE_FALSE(p(v));
}

TEST_CASE("makeTagPredicate: key+value match", "[selection][predicate]") {
    auto p = makeTagPredicate("subdetector", std::optional<std::string>{"tracker"});
    std::map<std::string, std::string> tags{{"subdetector", "tracker"}};
    std::map<std::string, std::string> otherTags{{"subdetector", "calorimeter"}};

    NodeView v;
    v.tags = &tags;
    REQUIRE(p(v));

    v.tags = &otherTags;
    REQUIRE_FALSE(p(v));
}

TEST_CASE("makeIsLeafPredicate: true only for leaves", "[selection][predicate]") {
    auto p = makeIsLeafPredicate();
    NodeView v;
    v.tags = &noTags;

    v.isLeaf = true;
    REQUIRE(p(v));

    v.isLeaf = false;
    REQUIRE_FALSE(p(v));
}

TEST_CASE("makeAndPredicate: short-circuits on first false", "[selection][predicate]") {
    auto always_false = makeNameGlobPredicate("NEVER");
    auto always_true = makeNameGlobPredicate("*");

    auto p = makeAndPredicate({always_true, always_false});
    NodeView v;
    v.name = "anything";
    v.tags = &noTags;
    REQUIRE_FALSE(p(v));

    auto p2 = makeAndPredicate({always_true, always_true});
    REQUIRE(p2(v));
}

TEST_CASE("makeOrPredicate: short-circuits on first true", "[selection][predicate]") {
    auto always_false = makeNameGlobPredicate("NEVER");
    auto always_true = makeNameGlobPredicate("*");

    auto p = makeOrPredicate({always_false, always_true});
    NodeView v;
    v.name = "anything";
    v.tags = &noTags;
    REQUIRE(p(v));

    auto p2 = makeOrPredicate({always_false, always_false});
    REQUIRE_FALSE(p2(v));
}

TEST_CASE("makeNotPredicate: inverts result", "[selection][predicate]") {
    auto base = makeNameGlobPredicate("foo");
    auto p = makeNotPredicate(std::move(base));
    NodeView v;
    v.tags = &noTags;

    v.name = "foo";
    REQUIRE_FALSE(p(v));

    v.name = "bar";
    REQUIRE(p(v));
}

// ── compilePredicate ──────────────────────────────────────────────────────────

TEST_CASE("compilePredicate: NameGlobPredicate", "[selection][predicate]") {
    PredicateExpr expr{NameGlobPredicate{"sensor*"}};
    auto p = compilePredicate(expr);
    NodeView v;
    v.tags = &noTags;

    v.name = "sensor01";
    REQUIRE(p(v));

    v.name = "module01";
    REQUIRE_FALSE(p(v));
}

TEST_CASE("compilePredicate: TagPredicate key-only", "[selection][predicate]") {
    PredicateExpr expr{TagPredicate{"sensitive", std::nullopt}};
    auto p = compilePredicate(expr);
    std::map<std::string, std::string> tags{{"sensitive", "true"}};
    NodeView v;
    v.tags = &tags;
    REQUIRE(p(v));
}

TEST_CASE("compilePredicate: AndPredicate compiles children recursively",
          "[selection][predicate]") {
    auto andPred = std::make_shared<AndPredicate>();
    andPred->operands.push_back(PredicateExpr{NameGlobPredicate{"sensor*"}});
    andPred->operands.push_back(PredicateExpr{TagPredicate{"sensitive", std::nullopt}});
    PredicateExpr expr{andPred};

    auto p = compilePredicate(expr);
    std::map<std::string, std::string> tags{{"sensitive", "true"}};
    std::map<std::string, std::string> emptyTags;
    NodeView v;

    v.name = "sensor01";
    v.tags = &tags;
    REQUIRE(p(v)); // name matches AND tag present

    v.tags = &emptyTags;
    REQUIRE_FALSE(p(v)); // tag missing
}

TEST_CASE("compilePredicate: NotPredicate wraps child", "[selection][predicate]") {
    auto notPred = std::make_shared<NotPredicate>();
    notPred->operand = PredicateExpr{NameGlobPredicate{"world"}};
    PredicateExpr expr{notPred};

    auto p = compilePredicate(expr);
    NodeView v;
    v.tags = &noTags;

    v.name = "world";
    REQUIRE_FALSE(p(v));

    v.name = "tracker";
    REQUIRE(p(v));
}

TEST_CASE("compilePredicate: IsLeafPredicate", "[selection][predicate]") {
    PredicateExpr expr{IsLeafPredicate{}};
    auto p = compilePredicate(expr);
    NodeView v;
    v.tags = &noTags;

    v.isLeaf = true;
    REQUIRE(p(v));

    v.isLeaf = false;
    REQUIRE_FALSE(p(v));
}

TEST_CASE("Compiled globs agree with an independent wildcard matcher", "[selection][predicate]") {
    // Exhaustively cover anchored literals, optional '/' after '**', repeated
    // stars, empty matches, and single-star slash boundaries.
    const auto words = [](std::string_view alphabet, std::size_t depth) {
        std::vector<std::string> values{""};
        std::size_t begin = 0;
        for (std::size_t n = 0; n < depth; ++n) {
            const auto end = values.size();
            for (std::size_t i = begin; i < end; ++i) {
                for (const char c : alphabet) {
                    values.push_back(values.at(i) + c);
                }
            }
            begin = end;
        }
        return values;
    };
    const auto reference = [](std::string_view pattern, std::string_view text) {
        std::vector<std::vector<bool>> matches(pattern.size() + 1,
                                               std::vector<bool>(text.size() + 1));
        matches.back().back() = true;
        for (std::size_t p = pattern.size(); p-- > 0;) {
            for (std::size_t t = text.size() + 1; t-- > 0;) {
                if (pattern.at(p) == '*') {
                    const bool doubleStar = p + 1 < pattern.size() && pattern.at(p + 1) == '*';
                    auto next = p + (doubleStar ? 2 : 1);
                    if (doubleStar && next < pattern.size() && pattern.at(next) == '/') {
                        ++next;
                    }
                    matches.at(p).at(t) = matches.at(next).at(t) ||
                                          (t < text.size() && (doubleStar || text.at(t) != '/') &&
                                           matches.at(p).at(t + 1));
                } else {
                    matches.at(p).at(t) = t < text.size() && pattern.at(p) == text.at(t) &&
                                          matches.at(p + 1).at(t + 1);
                }
            }
        }
        return static_cast<bool>(matches.front().front());
    };
    const auto texts = words("ab/", 4);
    for (const auto &pattern : words("a/*", 5)) {
        const auto predicate = makePathGlobPredicate(pattern);
        for (const auto &text : texts) {
            CAPTURE(pattern, text);
            NodeView view;
            view.path = text;
            REQUIRE(predicate(view) == reference(pattern, text));
        }
    }
}

TEST_CASE("Leading double-star shortcuts preserve path matching semantics",
          "[selection][predicate]") {
    REQUIRE(matchGlob("**/sensor", "biosensor"));
    REQUIRE(matchGlob("**/", "any/path"));
    REQUIRE(matchGlob("**//sensor", "prefix/sensor"));
    REQUIRE_FALSE(matchGlob("**//sensor", "sensor"));
    REQUIRE(matchGlob("**/PixelLayer*", "/world/xPixelLayer0"));
    REQUIRE_FALSE(matchGlob("**/PixelLayer*", "/world/PixelLayer0/stave0"));
    REQUIRE_FALSE(matchGlob("**/PixelLayer*", "/world/PixelLayer0/"));
    REQUIRE(
        matchGlob("**/PixelLayer*/stave*/**", "/xPixelLayer_wrong/x/PixelLayer0/stave0/sensor"));
    REQUIRE_FALSE(matchGlob("**/PixelLayer*/stave*", "/world/PixelLayer0/x/stave0"));
    REQUIRE(matchGlob("**/PixelLayer*/stave*", "/world/PixelLayer0/stave0"));
}

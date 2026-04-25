#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/selection/predicate.hpp>

#include <memory>

namespace nodehammer {

// ── matchGlob ─────────────────────────────────────────────────────────────────
//
// Iterative two-pointer matcher with fast-reject.
//
//   '**' — matches zero or more characters INCLUDING '/'.
//          A '/' immediately following '**' is consumed so that "**/foo"
//          matches both "foo" (zero-prefix) and "a/b/foo".
//   '*'  — matches zero or more characters that are NOT '/'.
//   all other characters — must match literally.
//
// The algorithm tracks the most recent '**' position as a restart point.
// When a mismatch occurs after a '*' or '**', it backtracks to the restart
// point and advances by one text character. This avoids recursion and runs
// in O(n*m) worst case with minimal overhead.
//
// CompiledGlob caches the pattern analysis — literal segments for fast-reject
// and specialized match modes — so hot paths (selection + tessellation rule
// evaluation) don't re-scan the pattern on every call.

namespace {

// Core two-pointer matcher — no fast-reject. Callers that have a compiled
// pattern apply fast-reject with precomputed segments before calling this.
bool matchGlobCore(std::string_view pattern, std::string_view text) {
    std::size_t pi = 0; // pattern index
    std::size_t ti = 0; // text index

    // Restart point for the most recent '**'.
    std::size_t dstarPat = std::string_view::npos; // pattern index after '**[/]'
    std::size_t dstarTxt = 0;                      // text index to retry from

    // Restart point for the most recent '*'.
    std::size_t starPat = std::string_view::npos;
    std::size_t starTxt = 0;

    while (ti < text.size() || pi < pattern.size()) {
        if (pi < pattern.size()) {
            // '**' — match any characters including '/'.
            if (pi + 1 < pattern.size() && pattern[pi] == '*' && pattern[pi + 1] == '*') {
                pi += 2;
                // Consume optional '/' after '**'.
                if (pi < pattern.size() && pattern[pi] == '/') {
                    ++pi;
                }
                dstarPat = pi;
                dstarTxt = ti;
                // Reset single-star state since '**' subsumes it.
                starPat = std::string_view::npos;
                continue;
            }

            // '*' — match any characters except '/'.
            if (pattern[pi] == '*') {
                ++pi;
                starPat = pi;
                starTxt = ti;
                continue;
            }

            // Literal match.
            if (ti < text.size() && pattern[pi] == text[ti]) {
                ++pi;
                ++ti;
                continue;
            }
        }

        // Mismatch — try backtracking to the most recent '*' first.
        if (starPat != std::string_view::npos && starTxt < text.size() && text[starTxt] != '/') {
            ++starTxt;
            ti = starTxt;
            pi = starPat;
            continue;
        }

        // Backtrack to the most recent '**'.
        if (dstarPat != std::string_view::npos && dstarTxt < text.size()) {
            ++dstarTxt;
            ti = dstarTxt;
            pi = dstarPat;
            // Reset single-star state.
            starPat = std::string_view::npos;
            continue;
        }

        // No wildcards to backtrack to — pattern does not match.
        return false;
    }

    return true;
}

// Precompiled form of a glob pattern, analyzed once at predicate-build time.
struct CompiledGlob {
    enum class Kind {
        MatchAll,         // pattern is "*", "**", or any string of consecutive '*'
        Literal,          // pattern has no '*' → equality check
        PrefixDoubleStar, // pattern is "<LITERAL>**" with nothing after → starts_with check
        Contains,         // pattern is "**<LITERAL>**" → text.find(LITERAL) check
        General,          // full two-pointer matcher with precomputed literal segments
    };

    Kind kind{Kind::General};
    std::string pattern;                    // held for General and Literal; storage for segments
    std::string_view literal;               // for Literal / PrefixDoubleStar
    std::vector<std::string_view> segments; // for General fast-reject (views into pattern)
};

// Analyze a pattern once, picking the most specific Kind we can prove correct.
// segments are views into `pattern`; we std::move the string after analysis so
// the views stay valid inside the owning CompiledGlob.
std::shared_ptr<const CompiledGlob> compileGlob(std::string pattern) {
    auto g = std::make_shared<CompiledGlob>();

    bool hasStar = false;
    bool allStar = !pattern.empty();
    for (char c : pattern) {
        if (c == '*') {
            hasStar = true;
        } else {
            allStar = false;
        }
    }

    // MatchAll only when the pattern contains a '**' run — a lone '*' must not
    // cross '/', so it falls through to the general matcher.
    if (allStar && pattern.size() >= 2) {
        g->kind = CompiledGlob::Kind::MatchAll;
        return g;
    }
    if (!hasStar) {
        g->kind = CompiledGlob::Kind::Literal;
        g->pattern = std::move(pattern);
        g->literal = g->pattern;
        return g;
    }

    // Detect "<LITERAL>**" — the common scope / prefix-glob form.
    // Requires: pattern ends with "**" and the prefix has no '*'.
    if (pattern.size() >= 2 && pattern[pattern.size() - 1] == '*' &&
        pattern[pattern.size() - 2] == '*') {
        std::string_view prefix{pattern.data(), pattern.size() - 2};
        if (prefix.find('*') == std::string_view::npos) {
            g->kind = CompiledGlob::Kind::PrefixDoubleStar;
            g->pattern = std::move(pattern);
            g->literal = std::string_view{g->pattern.data(), g->pattern.size() - 2};
            return g;
        }
    }

    // Detect "**<LITERAL>**" — unambiguously equivalent to text.find(LITERAL).
    // Requires LITERAL to have no '*' and not start with '/' (a leading '/'
    // would interact with the "consume trailing '/' after '**'" rule in the
    // matcher and give different semantics).
    if (pattern.size() >= 5 && pattern[0] == '*' && pattern[1] == '*' &&
        pattern[pattern.size() - 1] == '*' && pattern[pattern.size() - 2] == '*') {
        std::string_view middle{pattern.data() + 2, pattern.size() - 4};
        if (!middle.empty() && middle[0] != '/' && middle.find('*') == std::string_view::npos) {
            g->kind = CompiledGlob::Kind::Contains;
            g->pattern = std::move(pattern);
            g->literal = std::string_view{g->pattern.data() + 2, g->pattern.size() - 4};
            return g;
        }
    }

    // General case: precompute literal segments for fast-reject. Rejecting
    // mismatched (pattern, text) pairs via text.find is a big net win here —
    // removing this step regressed wasm runtime 2–3× in practice, because most
    // (rule × node) pairs don't match and fastReject catches them cheaply.
    g->kind = CompiledGlob::Kind::General;
    g->pattern = std::move(pattern);
    std::string_view p{g->pattern};
    std::size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '*') {
            if (i + 1 < p.size() && p[i + 1] == '*') {
                i += 2;
                if (i < p.size() && p[i] == '/') {
                    ++i;
                }
            } else {
                ++i;
            }
            continue;
        }
        std::size_t start = i;
        while (i < p.size() && p[i] != '*') {
            ++i;
        }
        if (i - start > 1) {
            g->segments.push_back(p.substr(start, i - start));
        }
    }
    return g;
}

bool matchCompiledGlob(const CompiledGlob &g, std::string_view text) {
    switch (g.kind) {
    case CompiledGlob::Kind::MatchAll:
        return true;
    case CompiledGlob::Kind::Literal:
        return text == g.literal;
    case CompiledGlob::Kind::PrefixDoubleStar:
        return text.starts_with(g.literal);
    case CompiledGlob::Kind::Contains:
        return text.find(g.literal) != std::string_view::npos;
    case CompiledGlob::Kind::General:
        for (const auto &seg : g.segments) {
            if (text.find(seg) == std::string_view::npos) {
                return false;
            }
        }
        return matchGlobCore(g.pattern, text);
    }
    return false; // unreachable
}

} // namespace

// Public API — preserves the test/CLI entry point. Production hot paths go
// through compiled predicates (compileGlob is called once at predicate build).
bool matchGlob(std::string_view pattern, std::string_view text) {
    const auto g = compileGlob(std::string{pattern});
    return matchCompiledGlob(*g, text);
}

// ── Factory functions ─────────────────────────────────────────────────────────

Predicate makeNameGlobPredicate(std::string pattern) {
    auto g = compileGlob(std::move(pattern));
    return [g = std::move(g)](const NodeView &v) -> bool { return matchCompiledGlob(*g, v.name); };
}

Predicate makePathGlobPredicate(std::string pattern) {
    auto g = compileGlob(std::move(pattern));
    return [g = std::move(g)](const NodeView &v) -> bool { return matchCompiledGlob(*g, v.path); };
}

Predicate makeMaterialGlobPredicate(std::string pattern) {
    auto g = compileGlob(std::move(pattern));
    return [g = std::move(g)](const NodeView &v) -> bool {
        return matchCompiledGlob(*g, v.materialName);
    };
}

Predicate makeTagPredicate(std::string key, std::optional<std::string> value) {
    return [k = std::move(key), val = std::move(value)](const NodeView &v) -> bool {
        if (v.tags == nullptr) {
            return false;
        }
        auto it = v.tags->find(k);
        if (it == v.tags->end()) {
            return false;
        }
        if (!val.has_value()) {
            return true; // key-exists check only
        }
        return it->second == *val;
    };
}

Predicate makeIsLeafPredicate() {
    return [](const NodeView &v) -> bool { return v.isLeaf; };
}

Predicate makeAndPredicate(std::vector<Predicate> operands) {
    return [ops = std::move(operands)](const NodeView &v) -> bool {
        for (const auto &op : ops) {
            if (!op(v)) {
                return false;
            }
        }
        return true; // vacuously true for empty operands
    };
}

Predicate makeOrPredicate(std::vector<Predicate> operands) {
    return [ops = std::move(operands)](const NodeView &v) -> bool {
        for (const auto &op : ops) {
            if (op(v)) {
                return true;
            }
        }
        return false; // vacuously false for empty operands
    };
}

Predicate makeNotPredicate(Predicate operand) {
    return [op = std::move(operand)](const NodeView &v) -> bool { return !op(v); };
}

// ── compilePredicate ──────────────────────────────────────────────────────────

Predicate compilePredicate(const PredicateExpr &expr) {
    return std::visit(
        detail::overloaded{
            [](const NameGlobPredicate &node) { return makeNameGlobPredicate(node.pattern); },
            [](const PathGlobPredicate &node) { return makePathGlobPredicate(node.pattern); },
            [](const MaterialGlobPredicate &node) {
                return makeMaterialGlobPredicate(node.pattern);
            },
            [](const TagPredicate &node) { return makeTagPredicate(node.key, node.value); },
            [](const IsLeafPredicate &) { return makeIsLeafPredicate(); },
            [](const BoolPredicate &node) -> Predicate {
                return [v = node.value](const NodeView &) { return v; };
            },
            [](const std::shared_ptr<AndPredicate> &node) -> Predicate {
                std::vector<Predicate> ops;
                ops.reserve(node->operands.size());
                for (const auto &operand : node->operands) {
                    ops.push_back(compilePredicate(operand));
                }
                return makeAndPredicate(std::move(ops));
            },
            [](const std::shared_ptr<OrPredicate> &node) {
                std::vector<Predicate> ops;
                ops.reserve(node->operands.size());
                for (const auto &operand : node->operands) {
                    ops.push_back(compilePredicate(operand));
                }
                return makeOrPredicate(std::move(ops));
            },
            [](const std::shared_ptr<NotPredicate> &node) {
                return makeNotPredicate(compilePredicate(node->operand));
            },
        },
        expr.data);
}

} // namespace nodehammer

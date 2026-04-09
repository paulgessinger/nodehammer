#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/selection/predicate.hpp>

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

namespace {

// Extract contiguous literal segments from a pattern (substrings between wildcards).
// Used for fast rejection: if any literal segment is not found in the text,
// the pattern cannot match.
bool fastReject(std::string_view pattern, std::string_view text) {
    std::size_t i = 0;
    while (i < pattern.size()) {
        // Skip wildcards and the optional '/' after '**'.
        if (pattern[i] == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                i += 2;
                if (i < pattern.size() && pattern[i] == '/') {
                    ++i;
                }
            } else {
                ++i;
            }
            continue;
        }
        // Collect literal segment
        std::size_t start = i;
        while (i < pattern.size() && pattern[i] != '*') {
            ++i;
        }
        std::string_view segment = pattern.substr(start, i - start);
        // A literal segment must appear somewhere in the text.
        if (segment.size() > 1 && text.find(segment) == std::string_view::npos) {
            return true; // reject
        }
    }
    return false;
}

} // namespace

bool matchGlob(std::string_view pattern, std::string_view text) {
    // Fast reject: check that all literal segments exist in the text.
    if (fastReject(pattern, text)) {
        return false;
    }

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

// ── Factory functions ─────────────────────────────────────────────────────────

Predicate makeNameGlobPredicate(std::string pattern) {
    return [p = std::move(pattern)](const NodeView &v) -> bool { return matchGlob(p, v.name); };
}

Predicate makePathGlobPredicate(std::string pattern) {
    return [p = std::move(pattern)](const NodeView &v) -> bool { return matchGlob(p, v.path); };
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

#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/selection/predicate.hpp>

namespace nodehammer {

// ── matchGlob ─────────────────────────────────────────────────────────────────
//
// Recursive backtracking matcher. Patterns are typically short (< 50 chars)
// so the recursion depth is bounded and exponential blowup is not a concern.
//
//   '**' — matches zero or more characters INCLUDING '/'.
//          A '/' immediately following '**' is consumed so that "**/foo"
//          matches both "foo" (zero-prefix) and "a/b/foo".
//   '*'  — matches zero or more characters that are NOT '/'.
//   all other characters — must match literally.

bool matchGlob(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
        return text.empty();
    }

    // "**" — greedy match over any characters (including '/').
    if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '*') {
        std::string_view rest = pattern.substr(2);
        // Consume an optional '/' separator that follows '**'.
        if (!rest.empty() && rest[0] == '/') {
            rest = rest.substr(1);
        }
        // Try matching rest against every suffix of text (skip == 0 → ** matches empty).
        for (std::size_t skip = 0; skip <= text.size(); ++skip) {
            if (matchGlob(rest, text.substr(skip))) {
                return true;
            }
        }
        return false;
    }

    // '*' — greedy match over characters that are NOT '/'.
    if (pattern[0] == '*') {
        std::string_view rest = pattern.substr(1);
        for (std::size_t skip = 0; skip <= text.size(); ++skip) {
            // If we just stepped over a '/', stop — '*' must not cross '/'.
            if (skip > 0 && text[skip - 1] == '/') {
                break;
            }
            if (matchGlob(rest, text.substr(skip))) {
                return true;
            }
        }
        return false;
    }

    // Literal character — must match exactly.
    if (text.empty() || pattern[0] != text[0]) {
        return false;
    }
    return matchGlob(pattern.substr(1), text.substr(1));
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

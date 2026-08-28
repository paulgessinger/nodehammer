#pragma once

// A minimal recursive JSON-shaped value, used for `extras` in both the config
// AST and the Render IR.
//
// This exists to keep nlohmann::json out of two very widely included headers.
// `extras` is *write-only* metadata: it is built from TOML and Lua, and emitted
// to TOML, glTF and the `dump-render` JSON. Nothing ever parses JSON text into
// it, and schemas/render.fbs deliberately omits it. Parsing is the hard half of
// a JSON library and where nlohmann's bulk lives, so carrying the whole of it to
// hold a handful of export annotations was a poor trade — it dominated the
// single-header amalgamation (docs/event-display-design.md §7) at ~38% of the
// generated lines, all of it unreachable.
//
// nlohmann is *not* going away: the semantic JSON importer/exporter does real
// parsing and keeps using it. Only `extras` moves.
//
// Three properties here are load-bearing, not incidental:
//
//   * The object alternative is a sorted `vector<pair<...>>`, not a `std::map`.
//     `std::vector<T>` with an incomplete `T` is explicitly permitted; `std::map`
//     with an incomplete mapped type is not, and only works by luck of the
//     implementation. A recursive value has no choice but the vector.
//   * Keys are kept *sorted*, because nlohmann's default object is a
//     `std::map` — i.e. alphabetical. Insertion order would silently reorder
//     keys in emitted glTF and TOML.
//   * Integer and floating-point are distinct alternatives. The glTF emitter
//     branches on the two to pick `tinygltf::Value(int)` vs
//     `tinygltf::Value(double)`; collapsing them changes the output.
//
// The scalar constructors are deliberately implicit, as nlohmann's were: the
// converters build nested literals leaf by leaf, and requiring `JsonValue{...}`
// at every leaf would obscure them. clang-tidy's google-explicit-constructor
// flags this; it is advisory here and the trade is intentional.

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nodehammer::detail {

/// A JSON value: null, bool, integer, double, string, array, or object.
class JsonValue {
  public:
    using Array = std::vector<JsonValue>;
    /// Sorted by key. Use `set()` to maintain that; do not push_back directly.
    using Object = std::vector<std::pair<std::string, JsonValue>>;

    using Storage =
        std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object>;

    // ── Construction ─────────────────────────────────────────────────────────

    /// Null.
    JsonValue() = default;

    JsonValue(bool b) : v_(b) {}

    /// Integral and floating-point are templated so `JsonValue{3}` and
    /// `JsonValue{1.5}` each pick one alternative rather than being ambiguous
    /// between int64_t, double and bool.
    template <typename T,
              std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    JsonValue(T i) : v_(static_cast<std::int64_t>(i)) {}

    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    JsonValue(T d) : v_(static_cast<double>(d)) {}

    JsonValue(std::string s) : v_(std::move(s)) {}
    /// Without this a string literal would decay to `const char *` and select
    /// the `bool` constructor.
    JsonValue(const char *s) : v_(std::string{s}) {}

    /// An array of `elems`, in the given order.
    static JsonValue makeArray(Array elems = {}) {
        JsonValue v;
        v.v_ = std::move(elems);
        return v;
    }

    /// An object holding `members`, sorted by key. Later duplicates win, which
    /// matches assigning the same key twice.
    static JsonValue makeObject(Object members = {}) {
        std::stable_sort(members.begin(), members.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        // stable_sort keeps equal keys in insertion order, so the last of a run
        // is the one to keep. Built into a fresh vector rather than compacted in
        // place: the erase-remove shape would self-move-assign the first element
        // onto itself, which is valid but leaves it unspecified -- in practice
        // an empty key.
        Object unique;
        unique.reserve(members.size());
        for (auto &member : members) {
            if (!unique.empty() && unique.back().first == member.first) {
                unique.back() = std::move(member);
            } else {
                unique.push_back(std::move(member));
            }
        }
        JsonValue v;
        v.v_ = std::move(unique);
        return v;
    }

    // ── Inspection ───────────────────────────────────────────────────────────

    const Storage &value() const { return v_; }

    bool isNull() const { return std::holds_alternative<std::monostate>(v_); }
    bool isObject() const { return std::holds_alternative<Object>(v_); }
    bool isArray() const { return std::holds_alternative<Array>(v_); }

    /// Null is empty; so is a container with no elements. Scalars are not,
    /// matching nlohmann's `empty()`.
    bool empty() const {
        if (const auto *a = std::get_if<Array>(&v_)) {
            return a->empty();
        }
        if (const auto *o = std::get_if<Object>(&v_)) {
            return o->empty();
        }
        return isNull();
    }

    /// Null for a non-container, so `size()` never lies about a scalar.
    std::size_t size() const {
        if (const auto *a = std::get_if<Array>(&v_)) {
            return a->size();
        }
        if (const auto *o = std::get_if<Object>(&v_)) {
            return o->size();
        }
        return 0;
    }

    // ── Typed access ─────────────────────────────────────────────────────────
    //
    // Pointer form returns null on a type mismatch; the value form throws. The
    // emitters use std::visit and need neither; these are for callers that
    // already know the shape (tests, and lookups by key).

    const bool *asBool() const { return std::get_if<bool>(&v_); }
    const std::int64_t *asInt() const { return std::get_if<std::int64_t>(&v_); }
    const double *asDouble() const { return std::get_if<double>(&v_); }
    const std::string *asString() const { return std::get_if<std::string>(&v_); }
    const Array *asArray() const { return std::get_if<Array>(&v_); }
    const Object *asObject() const { return std::get_if<Object>(&v_); }

    bool boolValue() const { return get<bool>("bool"); }
    std::int64_t intValue() const { return get<std::int64_t>("integer"); }
    double doubleValue() const { return get<double>("double"); }
    const std::string &stringValue() const { return get<std::string>("string"); }

    // ── Object access ────────────────────────────────────────────────────────

    /// The member named `key`, or nullptr if this is not an object or has no
    /// such member.
    const JsonValue *find(std::string_view key) const {
        const auto *obj = asObject();
        if (obj == nullptr) {
            return nullptr;
        }
        const auto it = std::lower_bound(
            obj->begin(), obj->end(), key,
            [](const auto &member, std::string_view k) { return member.first < k; });
        if (it == obj->end() || it->first != key) {
            return nullptr;
        }
        return &it->second;
    }

    /// As `find`, but throws when absent.
    const JsonValue &at(std::string_view key) const {
        const JsonValue *v = find(key);
        if (v == nullptr) {
            throw std::out_of_range{"JsonValue has no member '" + std::string{key} + "'"};
        }
        return *v;
    }

    bool contains(std::string_view key) const { return find(key) != nullptr; }

    // ── Mutation ─────────────────────────────────────────────────────────────

    /// Set `key` on an object, replacing any existing member and keeping the
    /// members sorted. Turns a null value into an object; overwrites any other
    /// scalar, which is what assigning a member to it means.
    void set(std::string key, JsonValue val) {
        if (!isObject()) {
            v_ = Object{};
        }
        auto &obj = std::get<Object>(v_);
        const auto it = std::lower_bound(
            obj.begin(), obj.end(), std::string_view{key},
            [](const auto &member, std::string_view k) { return member.first < k; });
        if (it != obj.end() && it->first == key) {
            it->second = std::move(val);
        } else {
            obj.insert(it, {std::move(key), std::move(val)});
        }
    }

    /// Append to an array, turning a null value into one.
    void push(JsonValue val) {
        if (!isArray()) {
            v_ = Array{};
        }
        std::get<Array>(v_).push_back(std::move(val));
    }

    friend bool operator==(const JsonValue &a, const JsonValue &b) { return a.v_ == b.v_; }

  private:
    template <typename T> const T &get(const char *what) const {
        const T *p = std::get_if<T>(&v_);
        if (p == nullptr) {
            throw std::runtime_error{std::string{"JsonValue is not a "} + what};
        }
        return *p;
    }

    Storage v_;
};

} // namespace nodehammer::detail

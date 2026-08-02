#pragma once

// The internal diagnostics list, over the *public* Diagnostic type.
//
// `Diagnostic` is not redeclared here. After step 1 deleted the `to_json`
// helpers the internal struct was field-for-field identical to the published
// one, so the API layer was converting a struct into a byte-identical struct and
// allocating three strings per item to do it. Dissolving the name rather than
// renaming around it is the move #41 §1 already makes for `ConfigResult`, and it
// means the connector amalgamation carries one definition instead of two.
//
// An internal header including a public one is the right direction: the public
// vocabulary is the more fundamental of the two, and nothing here is reachable
// from an installed header.
//
// What stays two types is the *list*, which is why this one is `List` rather
// than `diagnostics::List`. It is a mutable accumulator that hands out its
// `std::vector` by reference; the public one is opaque and immutable, and that
// coupling is exactly what it exists to avoid. Sharing a name with the public
// type was harmless while internal code could not see it — including its header
// made the two collide, which is the resolution #41's open item anticipated.

#include <nodehammer/diagnostics.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodehammer::diagnostics {

using Severity = Diagnostic::Severity;

[[nodiscard]] constexpr std::string_view severityName(Severity s) noexcept {
    switch (s) {
    case Severity::Debug:
        return "debug";
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    case Severity::Fatal:
        return "fatal";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool isFatal(const Diagnostic &d) noexcept {
    return d.severity == Severity::Fatal;
}

class List {
  public:
    void add(Diagnostic d) { items_.push_back(std::move(d)); }

    // std::string_view overloads accept literals, constexpr constants, and std::string alike.
    void debug(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Severity::Debug, std::string{code}, std::string{message}, std::string{context}});
    }
    void info(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Severity::Info, std::string{code}, std::string{message}, std::string{context}});
    }
    void warn(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Severity::Warning, std::string{code}, std::string{message}, std::string{context}});
    }
    void error(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Severity::Error, std::string{code}, std::string{message}, std::string{context}});
    }
    void fatal(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Severity::Fatal, std::string{code}, std::string{message}, std::string{context}});
    }

    void append(const List &other) {
        items_.reserve(items_.size() + other.items_.size());
        for (const auto &d : other.items_) {
            items_.push_back(d);
        }
    }

    [[nodiscard]] bool hasFatal() const noexcept {
        for (const auto &d : items_) {
            if (isFatal(d)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        for (const auto &d : items_) {
            if (d.severity >= Severity::Error) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<Diagnostic> &items() const noexcept { return items_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    /// Hand the items over wholesale. Now that both layers share `Diagnostic`,
    /// this is what lets the public list be produced by a move rather than by
    /// copying every string — see `api::asHandle`.
    [[nodiscard]] std::vector<Diagnostic> take() && noexcept { return std::move(items_); }

  private:
    std::vector<Diagnostic> items_;
};

} // namespace nodehammer::diagnostics

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
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodehammer {

/// The public list's state.
///
/// Defined here rather than in the API seam because producing one stopped being
/// a boundary operation: under docs/error-model.md every stage that fails
/// fatally hands its observations to `Error`, and stages are everywhere.
struct DiagnosticList::Impl {
    std::vector<Diagnostic> items;
};

/// Adopt a list the library built. Defined here for the same reason the state
/// is: this is where lists are made.
inline DiagnosticList::DiagnosticList(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

namespace diagnostics {

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

/// Hand this list's items to the public one.
///
/// Both sides hold `std::vector<Diagnostic>` — the same type, since this header
/// uses the published struct — so this moves rather than converts, and an empty
/// list allocates nothing.
[[nodiscard]] inline DiagnosticList asHandle(List &&src) {
    std::vector<Diagnostic> items = std::move(src).take();
    if (items.empty()) {
        return DiagnosticList{};
    }
    return DiagnosticList{
        std::make_shared<const DiagnosticList::Impl>(DiagnosticList::Impl{std::move(items)})};
}

/// For a list the caller still needs afterwards.
[[nodiscard]] inline DiagnosticList asHandle(const List &src) {
    List copy = src;
    return asHandle(std::move(copy));
}

/// Give up, when a collecting stage collected something fatal to what it
/// promised.
///
/// Some stages report rather than stop — the config loader above all, since
/// naming every problem in a document is the whole job. That is not a second
/// way of failing: it is one failure that happens to know several things. This
/// is where the collection becomes the exception.
///
/// The code and context come from the first error, since that is what a caller
/// would branch on; the message carries all of them, because a config with three
/// undefined material references should say so once rather than three calls in a
/// row. The whole list rides along on the exception, so nothing observed before
/// the failure is lost by it being fatal.
inline void throwIfErrors(const List &diags, std::string_view context) {
    const Diagnostic *first = nullptr;
    std::string message;
    for (const auto &d : diags.items()) {
        if (d.severity < Severity::Error) {
            continue;
        }
        if (first == nullptr) {
            first = &d;
            message = d.message;
            continue;
        }
        // Only once there is more than one does the message have to say which
        // code each part belongs to; a single failure already carries its code
        // on the exception.
        if (message == first->message) {
            message = first->code + ": " + first->message;
        }
        message += "; ";
        message += d.code;
        message += ": ";
        message += d.message;
    }
    if (first == nullptr) {
        return;
    }
    throw Error{first->code, message, first->context.empty() ? context : first->context,
                asHandle(diags)};
}

} // namespace diagnostics
} // namespace nodehammer

#pragma once

// What is left of the internal diagnostics layer once the list is a value: a
// severity alias, a name for each severity, and the one operation that turns a
// collection into the exception.
//
// There used to be two list types — a mutable accumulator here and an opaque
// handle in the public header, with a conversion between them. The split was
// justified by the opacity ("it hands out its `std::vector` by reference; the
// public one is opaque and immutable, and that coupling is exactly what it
// exists to avoid"), so publishing the list dissolved the reason for it. One
// type accumulates and is handed out, and it is `nodehammer::DiagnosticList`.
//
// An internal header including a public one is the right direction: the public
// vocabulary is the more fundamental of the two, and nothing here is reachable
// from an installed header.

#include <nodehammer/diagnostics.hpp>

#include <string>
#include <string_view>

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
inline void throwIfErrors(const DiagnosticList &diags, std::string_view context) {
    const Diagnostic *first = nullptr;
    std::string message;
    for (const auto &d : diags) {
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
    throw Error{first->code, message, first->context.empty() ? context : first->context, diags};
}

} // namespace nodehammer::diagnostics

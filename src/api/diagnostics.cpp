#include <api/handles.hpp>

namespace nodehammer {

// The list is a plain value now: every one of its members is inline in the
// public header and none is exported. What is left here is `Error`, whose
// members are exported because a consumer catches one.

// ── Error ────────────────────────────────────────────────────────────────────

Error::Error(std::string_view code, std::string_view message, std::string_view context)
    : std::runtime_error(std::string{message}), code_(code), context_(context) {}

// An empty list stays a null pointer rather than an allocation: most failures
// have nothing to carry, and the throw path is the last place to spend a
// heap allocation on nothing.
Error::Error(std::string_view code, std::string_view message, std::string_view context,
             DiagnosticList observed)
    : std::runtime_error(std::string{message}), code_(code), context_(context),
      observed_(observed.empty() ? nullptr
                                 : std::make_shared<const DiagnosticList>(std::move(observed))) {}

const std::string &Error::code() const noexcept { return code_; }

const std::string &Error::context() const noexcept { return context_; }

std::span<const Diagnostic> Error::observed() const noexcept {
    if (!observed_) {
        return {};
    }
    return std::span<const Diagnostic>{observed_->items()};
}

// `Fatal`, not `Error`: this is the one diagnostic in the system that reports a
// call which produced nothing, and the severity is what distinguishes it from
// the `Error`-severity entries that accompany a partial result.
Diagnostic Error::diagnostic() const {
    return Diagnostic{Diagnostic::Severity::Fatal, code_, what(), context_};
}

} // namespace nodehammer

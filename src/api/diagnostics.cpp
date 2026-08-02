#include <api/handles.hpp>

namespace nodehammer {

// Five accessors and nothing else. The special members are implicit: `impl_` is
// a `shared_ptr<const Impl>`, which copies and destroys without `Impl` being
// complete, so none of them has to be written here — or exported. Being members
// of the class, all five read that pointer directly; the adopting constructor
// in handles.hpp is the only other thing that touches it.
//
// A null `impl_` is the empty list, not an error state: it is what a
// default-constructed list holds, what `api::asHandle` produces for an empty one,
// and what a moved-from list is left with. Every accessor below answers for it.

bool DiagnosticList::hasErrors() const noexcept {
    if (!impl_) {
        return false;
    }
    for (const auto &d : impl_->items) {
        if (d.severity >= Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

bool DiagnosticList::empty() const noexcept { return impl_ == nullptr || impl_->items.empty(); }

std::size_t DiagnosticList::size() const noexcept { return impl_ ? impl_->items.size() : 0; }

// A null-null pair is a valid empty range, so range-for over an empty or
// moved-from list is well-defined rather than merely unlikely.
const Diagnostic *DiagnosticList::begin() const noexcept {
    return impl_ ? impl_->items.data() : nullptr;
}

const Diagnostic *DiagnosticList::end() const noexcept {
    return impl_ ? impl_->items.data() + impl_->items.size() : nullptr;
}

// ── Error ────────────────────────────────────────────────────────────────────

Error::Error(std::string_view code, std::string_view message, std::string_view context)
    : std::runtime_error(std::string{message}), code_(code), context_(context) {}

Error::Error(std::string_view code, std::string_view message, std::string_view context,
             DiagnosticList observed)
    : std::runtime_error(std::string{message}), code_(code), context_(context),
      observed_(std::move(observed)) {}

const std::string &Error::code() const noexcept { return code_; }

const std::string &Error::context() const noexcept { return context_; }

const DiagnosticList &Error::observed() const noexcept { return observed_; }

// `Fatal`, not `Error`: this is the one diagnostic in the system that reports a
// call which produced nothing, and the severity is what distinguishes it from
// the `Error`-severity entries that accompany a partial result.
Diagnostic Error::diagnostic() const {
    return Diagnostic{Diagnostic::Severity::Fatal, code_, what(), context_};
}

} // namespace nodehammer

#include <api/handles.hpp>

namespace nodehammer {

// Five accessors and nothing else. The special members are implicit: `impl` is
// a `shared_ptr<const Impl>`, which copies and destroys without `Impl` being
// complete, so none of them has to be written here — or exported.
//
// A null `impl` is the empty list, not an error state: it is what a
// default-constructed list holds, what `api::wrap` produces for an empty one,
// and what a moved-from list is left with. Every accessor below answers for it.

bool DiagnosticList::hasErrors() const noexcept {
    if (!impl) {
        return false;
    }
    for (const auto &d : impl->items) {
        if (d.severity >= Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

bool DiagnosticList::empty() const noexcept { return impl == nullptr || impl->items.empty(); }

std::size_t DiagnosticList::size() const noexcept { return impl ? impl->items.size() : 0; }

// A null-null pair is a valid empty range, so range-for over an empty or
// moved-from list is well-defined rather than merely unlikely.
const Diagnostic *DiagnosticList::begin() const noexcept {
    return impl ? impl->items.data() : nullptr;
}

const Diagnostic *DiagnosticList::end() const noexcept {
    return impl ? impl->items.data() + impl->items.size() : nullptr;
}

} // namespace nodehammer

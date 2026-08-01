#include <api/handles.hpp>

#include <memory>
#include <utility>

namespace nodehammer {

// Out-of-line because Impl is incomplete in the installed header. A copy
// deep-copies the items; a move leaves the source with no Impl, which every
// accessor below treats as an empty list rather than as an error.

DiagnosticList::DiagnosticList() : impl_(std::make_unique<Impl>()) {}

DiagnosticList::~DiagnosticList() = default;

DiagnosticList::DiagnosticList(const DiagnosticList &other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

DiagnosticList &DiagnosticList::operator=(const DiagnosticList &other) {
    if (this != &other) {
        impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    }
    return *this;
}

DiagnosticList::DiagnosticList(DiagnosticList &&other) noexcept = default;

DiagnosticList &DiagnosticList::operator=(DiagnosticList &&other) noexcept = default;

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

// A null-null pair is a valid empty range, so range-for over a moved-from list
// is well-defined rather than merely unlikely.
const Diagnostic *DiagnosticList::begin() const noexcept {
    return impl_ ? impl_->items.data() : nullptr;
}

const Diagnostic *DiagnosticList::end() const noexcept {
    return impl_ ? impl_->items.data() + impl_->items.size() : nullptr;
}

} // namespace nodehammer

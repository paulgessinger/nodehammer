#pragma once

// Diagnostics: how every verb reports what happened.
//
// Nothing in this API throws. A verb returns its result *and* a DiagnosticList,
// always both, and the list is always safe to inspect — a failed call returns an
// invalid handle rather than an exceptional control path. Internal code does
// throw (the FlatBuffers codecs in particular); the bridge catches at the
// boundary and converts (#41 principle 5).
//
// Tier A + Tier B: this header and semantic_scene.hpp are the whole of the
// amalgamated connector surface, so it deliberately pulls in nothing but the
// visibility macro and the standard library.

#include <nodehammer/api.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace nodehammer {

/// One thing the pipeline has to say. A value type on purpose: the connector
/// tier (#41 §3) carries this definition verbatim, and there is nothing here
/// worth hiding behind an accessor.
struct Diagnostic {
    /// Nested rather than a free `DiagnosticSeverity`, because it never appears
    /// in a signature without its enclosing type (#41 §4).
    enum class Severity { Debug, Info, Warning, Error, Fatal };

    Severity severity{Severity::Info};
    std::string code;    ///< Stable NH-series code, e.g. "NH0301"
    std::string message; ///< Human-readable text
    std::string context; ///< Optional: node path, source file, ...
};

/// An ordered, read-only sequence of diagnostics. Opaque: the internal list
/// hands out its `std::vector` by reference, and that coupling is exactly what
/// the public type exists to avoid.
///
/// Range-for works through the pointer pair, so `for (const auto &d : diags)`
/// is the intended traversal.
///
/// Immutable once produced, and shared rather than copied — the same handle
/// contract as `SemanticScene` and the rest, for the same reason: with no
/// mutator on the type, sharing is unobservable, so copying one is a pointer
/// bump instead of a deep copy of every string. An empty list (the common case,
/// since most calls succeed) owns nothing at all.
class DiagnosticList {
  public:
    /// True when any item is Error or Fatal — the one check a caller must not
    /// skip, since a verb reports failure by returning an invalid handle and
    /// saying why here.
    [[nodiscard]] NH_API bool hasErrors() const noexcept;

    [[nodiscard]] NH_API bool empty() const noexcept;
    [[nodiscard]] NH_API std::size_t size() const noexcept;

    [[nodiscard]] NH_API const Diagnostic *begin() const noexcept;
    [[nodiscard]] NH_API const Diagnostic *end() const noexcept;

    /// Opaque state.
    ///
    /// Public, and deliberately: what makes this handle opaque is that `Impl` is
    /// never defined in an installed header, not the access specifier. A
    /// consumer can copy or clear this pointer and nothing else — they cannot
    /// construct an `Impl`, cannot dereference one, and clearing it only
    /// produces the empty list a default-constructed one already is. Marking it
    /// private would buy exactly that nothing, at the price of a friend
    /// declaration on every handle so the library's own translation units could
    /// reach past it.
    ///
    /// `shared_ptr<const>`, so every special member stays implicit: copying and
    /// destroying one never needs `Impl` complete. A `unique_ptr` would force an
    /// out-of-line destructor — six exported entry points and a deep copy, for a
    /// type nobody can mutate.
    struct Impl;
    std::shared_ptr<const Impl> impl;
};

} // namespace nodehammer

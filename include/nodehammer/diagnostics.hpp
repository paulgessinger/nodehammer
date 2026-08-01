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

// The seam through which the implementation reaches a handle's internals.
// Declared, never defined here: an installed header exposes the name and
// nothing else, and being in a nested namespace it is internal by §1's rule
// ("everything declared directly in nodehammer:: is public"). One friend per
// handle is the whole of the ceremony.
namespace api {
struct Access;
}

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
class DiagnosticList {
  public:
    NH_API DiagnosticList();
    NH_API ~DiagnosticList();
    NH_API DiagnosticList(const DiagnosticList &other);
    NH_API DiagnosticList &operator=(const DiagnosticList &other);
    NH_API DiagnosticList(DiagnosticList &&other) noexcept;
    NH_API DiagnosticList &operator=(DiagnosticList &&other) noexcept;

    /// True when any item is Error or Fatal — the one check a caller must not
    /// skip, since a verb reports failure by returning an invalid handle and
    /// saying why here.
    [[nodiscard]] NH_API bool hasErrors() const noexcept;

    [[nodiscard]] NH_API bool empty() const noexcept;
    [[nodiscard]] NH_API std::size_t size() const noexcept;

    [[nodiscard]] NH_API const Diagnostic *begin() const noexcept;
    [[nodiscard]] NH_API const Diagnostic *end() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct api::Access;
};

} // namespace nodehammer

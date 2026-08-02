#pragma once

// How this API reports what happened — two channels, for two different things.
//
//   `Error` is thrown for input the call cannot act on: a format no backend
//   claims, a file that will not open, bytes that are not a scene, a handle that
//   refers to nothing. There is no result in these cases, only a reason, and an
//   exception is the one return path a caller cannot accidentally ignore.
//
//   `DiagnosticList` is returned alongside a result, and carries what the work
//   *observed* while producing it. Never that the call failed — a call that
//   could not proceed threw. Tessellation reporting NH0500 for one unmeshed
//   node still hands back a scene, and whether that scene is usable is the
//   caller's call to make, not this layer's.
//
// The split matters more than either half: collapsing it in one direction makes
// every ignorable observation fatal, and in the other makes an unusable call
// look like a successful one that happened to say something.
//
// The rule behind both, in full, is docs/error-model.md: a failure is fatal iff
// the call cannot deliver what its signature promises. Every failure this
// library can *name* is an `Error`, thrown where it happens rather than
// translated at a boundary. Anything else — `std::bad_alloc` above all —
// propagates unchanged, because no NH code would be true of it and because
// constructing an `Error` allocates.
//
// Tier A + Tier B: this header and semantic_scene.hpp are the whole of the
// amalgamated connector surface, so it deliberately pulls in nothing but the
// visibility macro and the standard library.

#include <nodehammer/visibility.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// An ordered sequence of diagnostics — a value, not a handle.
///
/// Transparent on purpose. `Diagnostic` has always been a published struct that
/// callers read by the byte, so the only thing an opaque wrapper hid was the
/// choice of container, and it cost a second list type, a conversion between
/// them, an `Impl`, and five exported symbols to hide a `std::vector` whose
/// element type is right there. This is that vector.
///
/// Every member is defined here and none is decorated, so the class adds
/// nothing to the export table: what crosses the boundary is the layout, which
/// this API had already committed to by returning `std::vector` and
/// `std::string` elsewhere — see the MSVC runtime guard in the package config,
/// which exists precisely because that commitment is real.
///
/// The same type accumulates inside the library and is handed out from it.
/// There is no "internal list" any more.
class DiagnosticList {
  public:
    // ── Recording ────────────────────────────────────────────────────────────
    //
    // An `Error`-severity entry means *part of the result is missing or wrong*
    // — never that the call failed. A stage that cannot deliver throws instead;
    // see docs/error-model.md. That distinction is the reason this type has no
    // `fatal`.

    void add(Diagnostic d) { items_.push_back(std::move(d)); }

    // `std::string_view` overloads take literals, the `codes::` constants and
    // `std::string` alike.
    void debug(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Diagnostic::Severity::Debug, std::string{code}, std::string{message},
             std::string{context}});
    }
    void info(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Diagnostic::Severity::Info, std::string{code}, std::string{message},
             std::string{context}});
    }
    void warn(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Diagnostic::Severity::Warning, std::string{code}, std::string{message},
             std::string{context}});
    }
    void error(std::string_view code, std::string_view message, std::string_view context = {}) {
        add({Diagnostic::Severity::Error, std::string{code}, std::string{message},
             std::string{context}});
    }

    void append(const DiagnosticList &other) {
        items_.insert(items_.end(), other.items_.begin(), other.items_.end());
    }

    // ── Reading ──────────────────────────────────────────────────────────────

    /// True when any entry is `Error`.
    ///
    /// Not a failure check — a call that could not proceed threw. This says the
    /// work reported something serious about the result it *did* produce, such
    /// as a shape it could not tessellate.
    [[nodiscard]] bool hasErrors() const noexcept;

    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    [[nodiscard]] const std::vector<Diagnostic> &items() const noexcept { return items_; }

    /// Range-for works through the pointer pair, so `for (const auto &d : diags)`
    /// is the intended traversal. A null-null pair is a valid empty range.
    [[nodiscard]] const Diagnostic *begin() const noexcept { return items_.data(); }
    [[nodiscard]] const Diagnostic *end() const noexcept { return items_.data() + items_.size(); }

    /// Hand the entries over wholesale, for a caller that is done with the list.
    [[nodiscard]] std::vector<Diagnostic> take() && noexcept { return std::move(items_); }

  private:
    std::vector<Diagnostic> items_;
};

/// The same question over a borrowed range — what `Error::observed()` hands
/// back, since an exception cannot afford to copy its list.
[[nodiscard]] inline bool hasErrors(std::span<const Diagnostic> diags) noexcept {
    for (const auto &d : diags) {
        if (d.severity >= Diagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

inline bool DiagnosticList::hasErrors() const noexcept {
    return nodehammer::hasErrors(std::span<const Diagnostic>{items_});
}

/// The one exception this API throws.
///
/// Carries the same vocabulary as a `Diagnostic` — an NH-series code, a message
/// and an optional context — because the failure is the same kind of thing and
/// only the channel differs. `what()` is the message, so a caller that catches
/// nothing more specific than `std::exception` still gets something useful.
class NH_API_TYPE Error : public std::runtime_error {
  public:
    NH_API Error(std::string_view code, std::string_view message, std::string_view context = {});

    /// For a call that had already observed things when it gave up.
    NH_API Error(std::string_view code, std::string_view message, std::string_view context,
                 DiagnosticList observed);

    /// The stable NH-series code, e.g. "NH0101".
    [[nodiscard]] NH_API const std::string &code() const noexcept;

    /// Where it happened — a path, a format name, a node. Often empty.
    [[nodiscard]] NH_API const std::string &context() const noexcept;

    /// Everything the call recorded before it failed, at every severity —
    /// including the error this exception reports, when the failure came from a
    /// stage that collects rather than stops.
    ///
    /// Without this a fatal failure would destroy the warnings that led up to
    /// it: a config with two dubious keys and one undefined material reference
    /// has three things to say, and the exception names one of them.
    ///
    /// A borrowed range rather than the list, and shared rather than owned
    /// inside, because throwing copy-initialises the exception object: a config
    /// that collected five hundred problems would deep-copy fifteen hundred
    /// strings on the failure path, and a copy that throws while an exception
    /// propagates is a `std::terminate`. Sharing keeps the throw a pointer bump,
    /// which is what makes carrying the list affordable at all.
    ///
    /// Valid as long as the exception is.
    [[nodiscard]] NH_API std::span<const Diagnostic> observed() const noexcept;

    /// The failure itself as a `Diagnostic`, for a caller that funnels both
    /// channels into one report. Severity `Fatal` — the one place that level
    /// appears, since a `DiagnosticList` this library returns never carries it.
    [[nodiscard]] NH_API Diagnostic diagnostic() const;

  private:
    std::string code_;
    std::string context_;
    std::shared_ptr<const DiagnosticList> observed_;
};

} // namespace nodehammer

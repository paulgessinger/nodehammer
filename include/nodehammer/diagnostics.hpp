#pragma once

// How this API reports what happened — two channels, for two different things.
//
//   `Error` is thrown for input the call cannot act on: a format no backend
//   claims, a file that will not open, bytes that are not a scene, a handle that
//   refers to nothing. There is no result in these cases, only a reason, and an
//   exception is the one return path a caller cannot accidentally ignore.
//
//   `DiagnosticList` is returned alongside a result, and carries what the work
//   *observed* while producing it — at any severity. Tessellation reporting
//   NH0500 for one unmeshed node still hands back a scene, and whether that is
//   usable is the caller's call to make, not this layer's.
//
// The split matters more than either half: collapsing it in one direction makes
// every ignorable observation fatal, and in the other makes an unusable call
// look like a successful one that happened to say something.
//
// Internal code throws its own types (the FlatBuffers codecs in particular);
// those are caught at the boundary and rethrown as `Error`, so exactly one
// exception type crosses the API.
//
// Tier A + Tier B: this header and semantic_scene.hpp are the whole of the
// amalgamated connector surface, so it deliberately pulls in nothing but the
// visibility macro and the standard library.

#include <nodehammer/visibility.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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
    /// True when any item is Error or Fatal.
    ///
    /// Not a failure check — a call that could not proceed threw. This says the
    /// work reported something it considers serious about the result it *did*
    /// produce, such as a shape it could not tessellate.
    [[nodiscard]] NH_API bool hasErrors() const noexcept;

    [[nodiscard]] NH_API bool empty() const noexcept;
    [[nodiscard]] NH_API std::size_t size() const noexcept;

    [[nodiscard]] NH_API const Diagnostic *begin() const noexcept;
    [[nodiscard]] NH_API const Diagnostic *end() const noexcept;

    /// Opaque state.
    ///
    /// What makes this handle opaque is that `Impl` is never *defined* in an
    /// installed header — it exists only in `src/api/handles.hpp`, which is not
    /// shipped. The member is private on top of that because the library needs
    /// exactly two things from the state and neither of them is the member:
    /// adopt a pointer the bridge just made, and read what one points at.
    /// Publishing the pointer itself would also publish the power to replace or
    /// clear it, and every handle here is immutable by contract.
    ///
    /// So: a constructor, and on the handles whose state is read from outside
    /// the class, a getter returning `const Impl &`. Both are declared here and
    /// *defined* in `handles.hpp`, which makes them undecorated and unexported —
    /// a consumer can name them, but cannot form an argument for the one or use
    /// the result of the other, and does not link either way. That is what
    /// replaces the friend declaration privacy would otherwise need on every
    /// handle.
    ///
    /// `shared_ptr<const>`, so every special member stays implicit: copying and
    /// destroying one never needs `Impl` complete. A `unique_ptr` would force an
    /// out-of-line destructor — six exported entry points and a deep copy, for a
    /// type nobody can mutate.
    struct Impl;

    DiagnosticList() noexcept = default;

    /// Adopt a list the library built. A null pointer is the empty list rather
    /// than an error state, which is why this type has no getter to go with the
    /// constructor: its own accessors read the member directly and all five
    /// answer for null, and nothing outside the class reads a list's state.
    explicit DiagnosticList(std::shared_ptr<const Impl> impl) noexcept;

  private:
    std::shared_ptr<const Impl> impl_;
};

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
    /// has three things to say, and the exception names one of them. Copying it
    /// is a pointer bump, so carrying it costs the exception nothing.
    [[nodiscard]] NH_API const DiagnosticList &observed() const noexcept;

    /// The failure itself as a `Diagnostic`, for a caller that funnels both
    /// channels into one report. Severity `Fatal` — the one place that level
    /// appears, since a `DiagnosticList` this library returns never carries it.
    [[nodiscard]] NH_API Diagnostic diagnostic() const;

  private:
    std::string code_;
    std::string context_;
    DiagnosticList observed_;
};

} // namespace nodehammer

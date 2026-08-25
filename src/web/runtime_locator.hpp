#pragma once

// Finding the wasm viewer runtime, and refusing one that does not match.
//
// The runtime is a *directory* (a shell, a worker script, three bundles and a
// stamp), not a program, and it is built by a different toolchain than the
// library that serves it. So the library cannot assume it exists, cannot assume
// where it is, and — this is the part that matters — cannot assume that a
// directory somebody pointed at is the one it was built against.
//
// Which is why the answer is a ladder and a check rather than a path. Every
// front door walks the same ladder: `nodehammer viewer serve`, the Python
// binding, and whatever else wants to serve the viewer. See
// docs/cli-and-web-viewer-plan.md Part 1.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nodehammer::web {

/// Where a candidate directory came from. The order is the precedence.
enum class RuntimeRung {
    /// An explicit path: `--web-assets DIR`.
    Explicit,
    /// `NODEHAMMER_WEB_ASSETS`.
    Environment,
    /// A directory the calling program supplied because it knows where its own
    /// runtime is — `RunOptions::webAssets`. The Python wheel is the case this
    /// exists for: `nodehammer-web` installs the runtime into site-packages,
    /// which no rung below could guess, and `nodehammer.cli.run` hands the path
    /// down rather than the library going looking for an interpreter.
    ///
    /// Below the environment variable on purpose. It is an automatic default,
    /// not a request, so a person testing a locally built runtime must be able
    /// to override it without uninstalling a package.
    Embedder,
    /// `<executable>/../share/nodehammer/web` — a native install tree that a
    /// wasm install tree was merged into.
    InstallTree,
};

/// Everything the ladder is *told*, as opposed to what it works out.
///
/// A struct rather than parameters because the rungs that can be supplied from
/// outside are the ones that grow: a version-keyed download cache is the next,
/// and adding it should not resign every caller.
/// Every member is defaulted, and not only for tidiness: naming a subset with a
/// designated initializer -- which is the whole point of the struct -- trips
/// GCC's -Wmissing-field-initializers, and this build treats that as an error.
/// A member with a default member initializer is exempt from it.
struct LadderInputs {
    /// Rung 1. Empty means "not given".
    std::filesystem::path explicitDir{};
    /// Rung 3. Empty means "the caller does not know of one".
    std::filesystem::path embedderDir{};
};

/// The name a message should use for a rung.
std::string_view describe(RuntimeRung rung) noexcept;

/// One rung, as walked. `rejection` is empty exactly when this rung answered.
struct RuntimeCandidate {
    RuntimeRung rung;
    std::filesystem::path path;
    std::string rejection;
};

/// A directory that passed every check, and what its stamp said.
struct RuntimeLocation {
    std::filesystem::path dir;
    int schema{};
    std::string version;
};

/// The schema id this library was compiled against.
///
/// A function rather than the macro, so the value is a symbol in the library
/// and a test cannot accidentally compare the macro to itself.
[[nodiscard]] int compiledSchema() noexcept;

/// Where the running executable lives, resolved through symlinks.
///
/// `nullopt` where the question has no answer: a wasm module has no executable
/// path, and a platform that does not answer is not an error — it costs one
/// rung, and the ladder says so.
[[nodiscard]] std::optional<std::filesystem::path> executablePath();

/// Walk the ladder and return the first directory that is a runtime this
/// library can serve.
///
/// Throws `Error` when no rung answers, with a message naming every directory it
/// looked at and why each one did not do — because the *normal* state of a
/// from-source build is that no rung answers at all (the wasm is a separate
/// Emscripten build), so a bare "not found" would read as a bug in the build the
/// user just made.
///
/// Every supplied rung is a deliberate statement: if one names a directory that
/// is not a usable runtime, that is the error, not a reason to keep looking.
/// Only the install tree is a guess, and only its absence is ordinary.
[[nodiscard]] RuntimeLocation locateRuntime(const LadderInputs &inputs = {});

/// The ladder as walked, rendered for a person: every rung, why each did not
/// answer, and what to do about it.
///
/// Separate from the `Error` on purpose. `Error::what()` is echoed twice by the
/// CLI's reporter — once as a diagnostic, once as a `command: message` summary —
/// which is fine for one line and unreadable for a page. So the exception stays
/// a sentence and this is printed once, on the failure path, by whoever has a
/// terminal to print to.
[[nodiscard]] std::string explainLadder(const std::vector<RuntimeCandidate> &ladder);

/// The ladder as walked, without throwing — every rung considered, in order.
///
/// `locateRuntime` is this plus "the first one that answered, or an `Error`
/// built from the whole list". Exposed because a caller that wants to *show*
/// the search (a `--verbose` path, a test) should not have to trigger a failure
/// to see it.
[[nodiscard]] std::vector<RuntimeCandidate> walkLadder(const LadderInputs &inputs = {});

} // namespace nodehammer::web

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
// front door walks the same ladder: `nodehammer viewer --web`, the Python
// binding, and whatever else wants to serve the viewer. See
// docs/cli-and-web-viewer-plan.md Part 1.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nodehammer::web {

/// Where a candidate directory came from. The order is the precedence.
enum class RuntimeRung {
    /// An explicit path: `--web-assets DIR`, or the parameter an embedder passed.
    Explicit,
    /// `NODEHAMMER_WEB_ASSETS`.
    Environment,
    /// `<executable>/../share/nodehammer/web` — a native install tree that a
    /// wasm install tree was merged into.
    InstallTree,
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
/// `explicitDir` is rung 1; empty means "not given". Throws `Error` when no rung
/// answers, with a message naming every directory it looked at and why each one
/// did not do — because the *normal* state of a from-source build is that no
/// rung answers at all (the wasm is a separate Emscripten build), so a bare
/// "not found" would read as a bug in the build the user just made.
///
/// Rungs 1 and 2 are deliberate statements: if either names a directory that is
/// not a usable runtime, that is the error, not a reason to keep looking. Only
/// rung 3 is a guess, and only its absence is ordinary.
[[nodiscard]] RuntimeLocation locateRuntime(const std::filesystem::path &explicitDir = {});

/// The ladder as walked, without throwing — every rung considered, in order.
///
/// `locateRuntime` is this plus "the first one that answered, or an `Error`
/// built from the whole list". Exposed because a caller that wants to *show*
/// the search (a `--verbose` path, a test) should not have to trigger a failure
/// to see it.
[[nodiscard]] std::vector<RuntimeCandidate>
walkLadder(const std::filesystem::path &explicitDir = {});

} // namespace nodehammer::web

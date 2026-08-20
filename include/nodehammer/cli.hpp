#pragma once

// The command line, as a function.
//
// `nodehammer convert ...` and `cli::run({"convert", ...})` are the same code
// reached two ways: the executable is a shim over this, and this is what a
// consumer of the installed library — the Python extension above all — calls
// when it wants the CLI without spawning one. There is no second dispatcher and
// no duplicated option table, which is the whole reason the command sources
// moved into the library.
//
// CLI11 does not appear here. The signature is `string_view`s and an `int`, so
// which parser the implementation uses stays an implementation detail; the
// registration surface that *does* name `CLI::App` lives in src/cli/, where an
// installed consumer cannot reach it.
//
// C++20, like every header in this directory: the library is built with C++23
// but an installed consumer must not be required to be. `std::span` and
// `std::string_view` are in bounds; `std::print` and `std::format` are not.

#include <nodehammer/visibility.hpp>

#include <span>
#include <string_view>

namespace nodehammer::cli {

/// What the caller is, beyond the arguments.
///
/// Everything here is a property of the *front door*, not of the work — which
/// is why the defaults are the conservative ones rather than the executable's.
/// `src/cli/main.cpp` opts in; an embedder gets the quiet behaviour without
/// having to know the option exists.
struct RunOptions {
    /// Page long output through `$PAGER` when stdout is a terminal.
    ///
    /// Off by default. The pager replaces file descriptor 1 for the duration
    /// and then blocks until the reader quits it — appropriate when a person
    /// typed the command, wrong when a program called a function. A TTY alone
    /// cannot tell those apart: an interactive interpreter has one.
    bool pager = false;
};

/// Run one command line. Returns the exit code the executable would have.
///
/// `args` excludes the program name — `{"convert", "--input", "a.gdml"}`, the
/// same slice `sys.argv[1:]` gives. Empty means "no command": the help text is
/// printed and 0 returned, never a window and never a default subcommand, since
/// what a bare invocation should do is a property of the front door.
///
/// It **returns** the failure rather than raising it. A command that could not
/// do its job reports the reason on stderr and answers non-zero, exactly as the
/// executable does; `Error` is reserved for a failure that escaped a command
/// body, which is a defect rather than a diagnosis. Nothing here calls
/// `std::exit`, so a caller keeps its process, its stack and its destructors.
///
/// Not thread-safe with respect to itself: commands write to stdout/stderr and
/// may read the current working directory.
[[nodiscard]] NH_API int run(std::span<const std::string_view> args,
                             const RunOptions &options = {});

} // namespace nodehammer::cli

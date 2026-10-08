// The executable, which is a front door and nothing else.
//
// Everything that used to be here — the App, the version flag, the registrars,
// the parse — is `cli::run` now, so that `nodehammer convert ...` and a caller
// of the installed library reach one implementation rather than two that have
// to be kept in step. What is left is the part that is genuinely about being a
// program: argv, the exit code, and the two policies below that a library
// caller must not inherit.

#include "cli_common.hpp"
#include "run_internal.hpp"

#include <nodehammer/cli.hpp>

#include <cstddef>
#include <print>
#include <string_view>
#include <vector>

int main(int argc, char **argv) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    nodehammer::CliOptions options;

    // A person typed this, so long output may page. The library default is off
    // (nodehammer/cli.hpp), because a TTY proves a terminal and not a reader:
    // an interactive interpreter has one too, and paging inside a caller's
    // process replaces its stdout and then blocks until somebody quits `less`.
    options.pager = true;

    // And a person typed this, so the commentary is for them. The library
    // default is on the other side (nodehammer/cli.hpp) for the same reason the
    // pager's is: a caller of `cli::run` wants the exit code and the answer on
    // stdout, not a running account of the work. `-q` turns it back off here.
    options.quiet = false;

#ifdef NH_WITH_VIEWER
    // No default subcommand. A bare `nodehammer` prints the help and returns 0,
    // here exactly as it does from `runCli({})` and from the wheel's console
    // script — one answer, on every platform and from every front door.
    //
    // It used to open the viewer when `argc == 1`, for double-clicking in a file
    // manager. The intent was right and the test was not: argument count cannot
    // tell a double-click from somebody typing `nodehammer` and expecting usage,
    // and it never fixed the Windows case it named — the subsystem is a field in
    // the PE header, so Explorer allocates a console for this binary whatever it
    // then chooses to run.
    //
    // The GUI entry point belongs to packaging instead, where each platform can
    // say it directly: an .app bundle, a .desktop carrying `Exec=nodehammer
    // viewer`, an installer shortcut pointing at a GUI-subsystem binary. See
    // issue #74. `viewer` remains a subcommand like any other.
#endif
    const std::vector<nodehammer::cli::detail::Registrar> extra{
#ifdef NH_WITH_VIEWER
        &nodehammer::cli::detail::registerCmdViewerNative,
#endif
#ifdef NH_WITH_DD4HEP
        &nodehammer::cli::detail::registerCmdDD4hepNative,
#endif
    };

    // Native registrations stay outside the callable CLI: the viewer owns a
    // window, and DD4hep's global-detector option and exit warning own process
    // policy. `runWith` supplies those additions to the shared dispatcher.
    try {
        return nodehammer::cli::detail::runWith(args, options, extra);
    } catch (const nodehammer::Error &e) {
        // The backstop, and deliberately here rather than inside `run`: every
        // command body runs within `runOrReport`, so an `Error` arriving here is
        // a missing wrapper. A program answers that by printing and exiting 1; a
        // library caller is better served by the exception reaching them.
        nodehammer::cli::printDiag(e.diagnostic());
        std::println(stderr, "nodehammer: {}", e.what());
        return 1;
    }
}

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

#include <array>
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

    nodehammer::cli::RunOptions options;

    // A person typed this, so long output may page. The library default is off
    // (nodehammer/cli.hpp), because a TTY proves a terminal and not a reader:
    // an interactive interpreter has one too, and paging inside a caller's
    // process replaces its stdout and then blocks until somebody quits `less`.
    options.pager = true;

#ifdef NH_WITH_VIEWER
    // No subcommand at all (double-clicked in a file manager, or run from a
    // terminal with zero args) -> default to `viewer` instead of the help text.
    // On Windows in particular the alternative prints to a console window that
    // closes the instant the process exits, so the app appears to silently do
    // nothing.
    //
    // This is why the default lives here and not in `run`: it is a claim about
    // how the program was launched, which a library call cannot make. It also
    // could not be written there — NH_WITH_VIEWER is applied to nodehammer_lib
    // and to neither nodehammer_shared nor the shared core objects, so in a
    // packaging build run.cpp is compiled once, without it, for both.
    if (args.empty()) {
        args.emplace_back("viewer");
    }

    const std::array extra{static_cast<nodehammer::cli::detail::Registrar>(
        &nodehammer::cli::detail::registerCmdViewerNative)};
#else
    const std::array<nodehammer::cli::detail::Registrar, 0> extra{};
#endif

    // `runWith` rather than `run`: the native viewer command constructs a
    // window, so it is compiled into this executable rather than into a shared
    // library that has to resolve every symbol it names. Handing it in is the
    // seam, and `CLI::App &` in its signature is exactly why that seam cannot
    // be a public header.
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

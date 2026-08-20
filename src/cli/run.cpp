#include "cli_common.hpp"
#include "run_internal.hpp"

#include <CLI/CLI.hpp>
#include <nodehammer/version.hpp>

#include <print>
#include <string>
#include <vector>

namespace nodehammer::cli {

namespace {

/// Parse and dispatch, turning everything a command can throw into a code.
///
/// This is what `CLI11_PARSE` used to do, written out. The macro expands to a
/// `catch` around a `return`, so it can only ever live in the function it
/// returns from — which was `main`, and is the reason there was no entry point
/// to call from anywhere else.
///
/// Three clauses rather than one:
///
///   CLI::ParseError    a bad flag, or `--help`. `App::exit` prints and picks
///                      the code, including 0 for help.
///   CommandFailure     a command reported its failure and chose a code.
///   nodehammer::Error  deliberately *not* caught here. A command body that
///                      escapes its `runOrReport` is a defect, and the front
///                      door is where the caller decides what a defect looks
///                      like: the executable prints it (main.cpp), while an
///                      API caller gets the exception.
int parseAndDispatch(CLI::App &app, std::span<const std::string_view> args) {
    // The argc/argv overload rather than the vector one: CLI11's
    // `parse(std::vector<std::string> &)` "expects a reversed vector"
    // (CLI/App.hpp), which is a trap for the sake of nothing here, and only the
    // argv form wants an element 0 at all. It is synthetic, and `runWith` has
    // already set the name explicitly, so the two agree and neither depends on
    // what the caller's argv[0] happened to be.
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.emplace_back("nodehammer");
    for (const auto arg : args) {
        owned.emplace_back(arg);
    }

    std::vector<const char *> argv;
    argv.reserve(owned.size());
    for (const auto &arg : owned) {
        argv.push_back(arg.c_str());
    }

    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    } catch (const detail::CommandFailure &failure) {
        return failure.code;
    }
    return 0;
}

} // namespace

namespace detail {

int runWith(std::span<const std::string_view> args, const RunOptions &options,
            std::span<const Registrar> extra) {
    CLI::App app{"nodehammer -- HEP geometry conversion pipeline"};
    app.require_subcommand(1);

    // Named here rather than left to argv[0], because the help below is printed
    // on a path that never parses -- and CLI11 takes the name during the parse.
    // Without this, `nodehammer` with no arguments in a build with no viewer
    // answers "Usage: [OPTIONS] SUBCOMMAND", with nothing to type.
    //
    // Fixing it here rather than at the print also settles the general case: the
    // name is a property of the program being described, not of how the caller
    // spelled its path, so `nh.cli.run([...])` under an interpreter does not
    // produce help that tells the reader to run `python3`.
    app.name("nodehammer");

    // set_version_flag rather than add_flag_callback, and the difference is the
    // whole bug: a flag callback runs at the *end* of parsing, by which point
    // require_subcommand(1) above has already failed, so `nodehammer --version`
    // answered "A subcommand is required" and never printed anything. CLI11's
    // version flag is checked while parsing and short-circuits, which is what
    // makes it work in a build that requires a subcommand.
    //
    // It is also what makes the answer identical with and without the viewer:
    // the flag is handled before the empty-args branch below, so asking for the
    // version never depends on which subcommands exist.
    app.set_version_flag("-V,--version",
                         std::string{"nodehammer "} + std::string{nodehammer::VERSION},
                         "Print version and exit");

    registerCmdConvert(app, options);
    registerCmdInspect(app, options);
    registerCmdValidateConfig(app, options);
    registerCmdConfigFlatten(app, options);
    registerCmdDumpSemantic(app, options);
    registerCmdDumpRender(app, options);
    registerCmdConfigLua(app, options);
    for (const auto registrar : extra) {
        registrar(app, options);
    }

    // No arguments at all: print the help and succeed.
    //
    // Printed rather than re-parsed as `{"--help"}` — a second parse to reach
    // text CLI11 will hand over directly is work for its own sake.
    //
    // Deliberately not "open the viewer". That default belongs to the
    // executable, which is where a bare invocation can mean a double-click, and
    // it is expressed there (src/cli/main.cpp). Expressing it here would need
    // NH_WITH_VIEWER, which under a shared build is not even *true* in this
    // file: the define is applied to nodehammer_lib and to neither
    // nodehammer_shared nor the shared core objects, so the core is compiled
    // once, without it, for both.
    if (args.empty()) {
        std::print("{}", app.help());
        return 0;
    }

    return parseAndDispatch(app, args);
}

} // namespace detail

int run(std::span<const std::string_view> args, const RunOptions &options) {
    return detail::runWith(args, options, {});
}

} // namespace nodehammer::cli

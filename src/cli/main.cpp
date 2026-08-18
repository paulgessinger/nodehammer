#include <CLI/CLI.hpp>
#include <nodehammer/version.hpp>

#include <string>

// Forward declarations — each command is implemented in its own translation unit.
void registerCmdConvert(CLI::App &app);
void registerCmdInspect(CLI::App &app);
void registerCmdValidateConfig(CLI::App &app);
void registerCmdConfigFlatten(CLI::App &app);
void registerCmdDumpSemantic(CLI::App &app);
void registerCmdDumpRender(CLI::App &app);
void registerCmdConfigLua(CLI::App &app);
#ifdef NH_WITH_VIEWER
void registerCmdViewer(CLI::App &app);
#endif

int main(int argc, char **argv) {
    CLI::App app{"nodehammer -- HEP geometry conversion pipeline"};
    app.require_subcommand(1);

    // set_version_flag rather than add_flag_callback, and the difference is the
    // whole bug: a flag callback runs at the *end* of parsing, by which point
    // require_subcommand(1) above has already failed, so `nodehammer --version`
    // answered "A subcommand is required" and never printed anything. CLI11's
    // version flag is checked while parsing and short-circuits, which is what
    // makes it work in a build that requires a subcommand.
    //
    // It is also what makes the answer identical with and without the viewer:
    // the flag is handled before either of the argc == 1 fallbacks below, so
    // asking for the version never depends on which subcommands exist.
    app.set_version_flag("-V,--version",
                         std::string{"nodehammer "} + std::string{nodehammer::VERSION},
                         "Print version and exit");

    registerCmdConvert(app);
    registerCmdInspect(app);
    registerCmdValidateConfig(app);
    registerCmdConfigFlatten(app);
    registerCmdDumpSemantic(app);
    registerCmdDumpRender(app);
    registerCmdConfigLua(app);
#ifdef NH_WITH_VIEWER
    registerCmdViewer(app);

    // No subcommand at all (double-clicked in a file manager, or run from a
    // terminal with zero args) -> default to `viewer` instead of CLI11's
    // "a subcommand is required" error. On Windows in particular that error
    // prints to a console window that closes the instant the process exits,
    // so the app appears to silently do nothing.
    if (argc == 1) {
        char viewerArg[] = "viewer";
        char *defaultArgv[] = {argv[0], viewerArg};
        CLI11_PARSE(app, 2, defaultArgv);
        return 0;
    }
#else
    // No viewer to fall back to in this build — show the help text instead
    // of CLI11's "a subcommand is required" error, so a bare double-click
    // (or a zero-arg invocation) leaves the user with something readable
    // rather than a console that opens and closes instantly.
    if (argc == 1) {
        char helpArg[] = "--help";
        char *defaultArgv[] = {argv[0], helpArg};
        CLI11_PARSE(app, 2, defaultArgv);
        return 0;
    }
#endif

    CLI11_PARSE(app, argc, argv);
    return 0;
}

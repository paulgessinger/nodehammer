#include <CLI/CLI.hpp>
#include <nodehammer/version.hpp>

#include <print>

// Forward declarations — each command is implemented in its own translation unit.
void registerCmdConvert(CLI::App &app);
void registerCmdInspect(CLI::App &app);
void registerCmdValidateConfig(CLI::App &app);
void registerCmdConfigFlatten(CLI::App &app);
void registerCmdDumpSemantic(CLI::App &app);
void registerCmdDumpRender(CLI::App &app);
#ifdef NH_WITH_VIEWER
void registerCmdViewer(CLI::App &app);
#endif

int main(int argc, char **argv) {
    CLI::App app{"nodehammer — HEP geometry conversion pipeline"};
    app.require_subcommand(1);

    // --version flag: print and exit
    app.add_flag_callback(
        "-V,--version",
        [] {
            std::println("nodehammer {}", nodehammer::VERSION);
            throw CLI::Success{};
        },
        "Print version and exit");

    registerCmdConvert(app);
    registerCmdInspect(app);
    registerCmdValidateConfig(app);
    registerCmdConfigFlatten(app);
    registerCmdDumpSemantic(app);
    registerCmdDumpRender(app);
#ifdef NH_WITH_VIEWER
    registerCmdViewer(app);
#endif

    CLI11_PARSE(app, argc, argv);
    return 0;
}

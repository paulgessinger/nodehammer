#include <CLI/CLI.hpp>
#include <nodehammer/version.hpp>

#include <print>

// Forward declarations — each command is implemented in its own translation unit.
void register_cmd_convert(CLI::App &app);
void register_cmd_inspect(CLI::App &app);
void register_cmd_validate_config(CLI::App &app);
void register_cmd_dump_semantic(CLI::App &app);
void register_cmd_dump_render(CLI::App &app);
#ifdef NH_WITH_VIEWER
void register_cmd_viewer(CLI::App &app);
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

    register_cmd_convert(app);
    register_cmd_inspect(app);
    register_cmd_validate_config(app);
    register_cmd_dump_semantic(app);
    register_cmd_dump_render(app);
#ifdef NH_WITH_VIEWER
    register_cmd_viewer(app);
#endif

    CLI11_PARSE(app, argc, argv);
    return 0;
}

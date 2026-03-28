#include <CLI/CLI.hpp>
#include <print>

void register_cmd_inspect(CLI::App &app) {
    auto *sub = app.add_subcommand("inspect", "Inspect a geometry file and print a summary");

    auto *input = sub->add_option("-i,--input", "Input geometry file")->required();
    auto *input_format =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");

    (void)input;
    (void)input_format;

    sub->callback([] { std::println(stderr, "nodehammer inspect: not yet implemented"); });
}

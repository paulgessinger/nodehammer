#include <CLI/CLI.hpp>
#include <print>

void register_cmd_convert(CLI::App &app) {
    auto *sub = app.add_subcommand("convert", "Convert a geometry file to a render format");

    auto *input = sub->add_option("-i,--input", "Input geometry file")->required();
    auto *input_format =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *config = sub->add_option("-c,--config", "TOML config file");
    auto *output = sub->add_option("-o,--output", "Output file")->required();
    auto *out_format = sub->add_option("--output-format",
                                       "Output format (auto-detected from extension if omitted)");
    sub->add_flag("--strict", "Treat warnings as errors");

    // Suppress unused-variable warnings until implementation lands
    (void)input;
    (void)input_format;
    (void)config;
    (void)output;
    (void)out_format;

    sub->callback([] { std::println(stderr, "nodehammer convert: not yet implemented"); });
}

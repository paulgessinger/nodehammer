#include <CLI/CLI.hpp>
#include <print>

void register_cmd_dump_render(CLI::App& app)
{
    auto* sub = app.add_subcommand("dump-render", "Dump the render IR of a geometry as JSON");

    auto* input        = sub->add_option("-i,--input",         "Input geometry file")->required();
    auto* input_format = sub->add_option("--input-format",     "Input format (auto-detected from extension if omitted)");
    auto* config       = sub->add_option("-c,--config",        "TOML config file");
    auto* output       = sub->add_option("-o,--output",        "Output JSON file (default: stdout)");

    (void)input; (void)input_format; (void)config; (void)output;

    sub->callback([] {
        std::println(stderr, "nodehammer dump-render: not yet implemented");
    });
}

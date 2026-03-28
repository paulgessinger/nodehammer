#include <CLI/CLI.hpp>
#include <print>

void register_cmd_dump_semantic(CLI::App& app)
{
    auto* sub = app.add_subcommand("dump-semantic", "Dump the semantic IR of a geometry as JSON");

    auto* input        = sub->add_option("-i,--input",         "Input geometry file")->required();
    auto* input_format = sub->add_option("--input-format",     "Input format (auto-detected from extension if omitted)");
    auto* output       = sub->add_option("-o,--output",        "Output JSON file (default: stdout)");

    (void)input; (void)input_format; (void)output;

    sub->callback([] {
        std::println(stderr, "nodehammer dump-semantic: not yet implemented");
    });
}

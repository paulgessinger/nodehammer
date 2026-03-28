#include <CLI/CLI.hpp>
#include <print>

void register_cmd_validate_config(CLI::App& app)
{
    auto* sub = app.add_subcommand("validate-config", "Validate a TOML config file");

    auto* config = sub->add_option("-c,--config", "TOML config file")->required();
    (void)config;

    sub->callback([] {
        std::println(stderr, "nodehammer validate-config: not yet implemented");
    });
}

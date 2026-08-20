#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <print>
#include <string>

void registerCmdValidateConfig(CLI::App &app) {
    auto *sub = app.add_subcommand("validate-config", "Validate a TOML config file");

    auto *configOpt = sub->add_option("-c,--config", "TOML config file")->required();

    sub->callback([=] {
        nodehammer::cli::runOrReport("validate-config", [&] {
            std::string configPath;
            configOpt->results(configPath);

            // The collecting face, deliberately: this command promises a report,
            // so a document that will not parse is its answer rather than its
            // failure (docs/error-model.md).
            auto result = nodehammer::config::ConfigLoader::collectFromFile(configPath);
            nodehammer::cli::printDiags(result.diags);

            if (result.diags.hasErrors()) {
                std::println(stderr, "config: INVALID (parse errors)");
                return;
            }

            auto validationDiags = nodehammer::config::ConfigValidator::validate(result.config);
            nodehammer::cli::printDiags(validationDiags);

            if (validationDiags.hasErrors()) {
                std::println(stderr, "config: INVALID");
            } else {
                std::println("config: OK");
            }
        });
    });
}

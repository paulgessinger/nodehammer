#include <CLI/CLI.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <print>
#include <string>

void register_cmd_validate_config(CLI::App &app) {
    auto *sub = app.add_subcommand("validate-config", "Validate a TOML config file");

    auto *configOpt = sub->add_option("-c,--config", "TOML config file")->required();

    auto printDiags = [](const nodehammer::DiagnosticList &diags) {
        for (const auto &d : diags.items()) {
            std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                         d.message);
            if (!d.context.empty()) {
                std::println(stderr, "       at {}", d.context);
            }
        }
    };

    sub->callback([=] {
        std::string configPath;
        configOpt->results(configPath);

        auto result = nodehammer::ConfigLoader::loadFromFile(configPath);
        printDiags(result.diags);

        if (result.diags.hasErrors()) {
            std::println(stderr, "config: INVALID (parse errors)");
            return;
        }

        auto validationDiags = nodehammer::ConfigValidator::validate(result.config);
        printDiags(validationDiags);

        if (validationDiags.hasErrors()) {
            std::println(stderr, "config: INVALID");
        } else {
            std::println("config: OK");
        }
    });
}

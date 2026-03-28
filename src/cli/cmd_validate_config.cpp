#include <CLI/CLI.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <print>
#include <string>

void register_cmd_validate_config(CLI::App &app) {
    auto *sub = app.add_subcommand("validate-config", "Validate a TOML config file");

    auto *configOpt = sub->add_option("-c,--config", "TOML config file")->required();

    sub->callback([=] {
        std::string configPath;
        configOpt->results(configPath);

        auto result = nodehammer::ConfigLoader::loadFromFile(configPath);
        if (!result) {
            for (const auto &d : result.error().items()) {
                std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                             d.message);
                if (!d.context.empty()) {
                    std::println(stderr, "       at {}", d.context);
                }
            }
            std::println(stderr, "config: INVALID (parse errors)");
            return;
        }

        auto validationDiags = nodehammer::ConfigValidator::validate(*result);
        for (const auto &d : validationDiags.items()) {
            std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                         d.message);
            if (!d.context.empty()) {
                std::println(stderr, "       at {}", d.context);
            }
        }

        if (validationDiags.hasErrors()) {
            std::println(stderr, "config: INVALID");
        } else {
            std::println("config: OK");
        }
    });
}

#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <config/config_writer.hpp>

#include <fstream>
#include <memory>
#include <print>
#include <string>

void registerCmdConfigFlatten(CLI::App &app) {
    auto *sub = app.add_subcommand(
        "config-flatten", "Inline all `include = [...]` references into a single self-contained "
                          "TOML file. Output is round-trippable through ConfigLoader.");

    auto *configOpt = sub->add_option("-c,--config", "Input TOML config file")->required();
    auto *outOpt = sub->add_option("-o,--output", "Output path (defaults to stdout if omitted)");
    auto validate = std::make_shared<bool>(true);
    sub->add_flag("!--no-validate", *validate,
                  "Skip ConfigValidator after parse (validation is on by default)");

    sub->callback([=] {
        nodehammer::cli::runOrReport("config-flatten", [&] {
            std::string configPath;
            configOpt->results(configPath);

            // ConfigLoader::loadFromFile already resolves the include tree
            // recursively into a single NHConfig — that's exactly the merged
            // form we want to serialise back out.
            auto result = nodehammer::config::ConfigLoader::loadFromFile(configPath);
            nodehammer::cli::printDiags(result.diags);

            if (*validate) {
                auto validationDiags = nodehammer::config::ConfigValidator::validate(result.config);
                nodehammer::cli::reportOrThrow(validationDiags, configPath);
            }

            const std::string toml = nodehammer::config::configToToml(result.config);

            if (*outOpt) {
                std::string outPath;
                outOpt->results(outPath);
                std::ofstream out{outPath};
                if (!out) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliFileOpen,
                                            std::format("could not open '{}' for writing", outPath),
                                            outPath};
                }
                out << toml;
                std::println(stderr, "config-flatten: wrote {} ({} bytes)", outPath, toml.size());
            } else {
                std::print("{}", toml);
            }
        });
    });
}

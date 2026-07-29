#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <config/config_validator.hpp>
#include <config/config_writer.hpp>
#include <lua/lua_config.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <sstream>
#include <string>

void registerCmdConfigLua(CLI::App &app) {
    auto *sub = app.add_subcommand(
        "config-lua", "Evaluate a Lua config script and emit flattened TOML. Output is "
                      "round-trippable through ConfigLoader.");

    auto *configOpt = sub->add_option("-c,--config", "Input Lua config script")->required();
    auto *outOpt = sub->add_option("-o,--output", "Output path (defaults to stdout if omitted)");
    auto validate = std::make_shared<bool>(true);
    sub->add_flag("!--no-validate", *validate,
                  "Skip ConfigValidator after evaluation (validation is on by default)");

    sub->callback([=] {
        std::string configPath;
        configOpt->results(configPath);

        std::ifstream in{configPath, std::ios::binary};
        if (!in) {
            std::println(stderr, "config-lua: could not open '{}' for reading", configPath);
            std::exit(1);
        }
        std::ostringstream buf;
        buf << in.rdbuf();

        // The script's include()/use() paths resolve relative to the script's
        // own directory, so pass that as the base.
        const std::filesystem::path scriptPath{configPath};
        const std::filesystem::path baseDir =
            scriptPath.has_parent_path() ? scriptPath.parent_path() : std::filesystem::path{"."};

        auto result = nodehammer::evalLuaConfig(buf.str(), configPath, baseDir);
        nodehammer::cli::printDiags(result.diags);
        if (result.diags.hasErrors()) {
            std::println(stderr, "config-lua: evaluation failed");
            std::exit(1);
        }

        if (*validate) {
            auto validationDiags = nodehammer::ConfigValidator::validate(result.config);
            nodehammer::cli::printDiags(validationDiags);
            if (validationDiags.hasErrors()) {
                std::println(stderr, "config-lua: validation failed");
                std::exit(1);
            }
        }

        const std::string toml = nodehammer::configToToml(result.config);

        if (*outOpt) {
            std::string outPath;
            outOpt->results(outPath);
            std::ofstream out{outPath};
            if (!out) {
                std::println(stderr, "config-lua: could not open '{}' for writing", outPath);
                std::exit(1);
            }
            out << toml;
            std::println(stderr, "config-lua: wrote {} ({} bytes)", outPath, toml.size());
        } else {
            std::print("{}", toml);
        }
    });
}

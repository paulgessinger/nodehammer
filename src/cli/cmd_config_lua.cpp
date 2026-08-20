#include "cli_common.hpp"
#include "run_internal.hpp"

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

namespace nodehammer::cli::detail {

void registerCmdConfigLua(CLI::App &app, const RunOptions &) {
    auto *sub = app.add_subcommand(
        "config-lua", "Evaluate a Lua config script and emit flattened TOML. Output is "
                      "round-trippable through ConfigLoader.");

    auto *configOpt = sub->add_option("-c,--config", "Input Lua config script")->required();
    auto *outOpt = sub->add_option("-o,--output", "Output path (defaults to stdout if omitted)");
    auto validate = std::make_shared<bool>(true);
    sub->add_flag("!--no-validate", *validate,
                  "Skip ConfigValidator after evaluation (validation is on by default)");

    sub->callback([=] {
        runOrReport("config-lua", [&] {
            std::string configPath;
            configOpt->results(configPath);

            std::ifstream in{configPath, std::ios::binary};
            if (!in) {
                throw nodehammer::Error{nodehammer::codes::kFatalCliFileOpen,
                                        std::format("could not open '{}' for reading", configPath),
                                        configPath};
            }
            std::ostringstream buf;
            buf << in.rdbuf();

            // The path the user typed is the root key, so include()/use()
            // resolve against the script's own directory — and a bare name
            // roots at the working directory, which is the application-level
            // choice this command is entitled to make on the user's behalf
            // because they named the path relative to it.
            const std::string rootKey =
                std::filesystem::path{configPath}.lexically_normal().generic_string();

            auto result = nodehammer::lua::evalLuaConfig(
                buf.str(), rootKey, nodehammer::config::ConfigLoader::filesystemFetcher());
            reportOrThrow(result.diags, configPath);

            if (*validate) {
                auto validationDiags = nodehammer::config::ConfigValidator::validate(result.config);
                reportOrThrow(validationDiags, configPath);
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
                std::println(stderr, "config-lua: wrote {} ({} bytes)", outPath, toml.size());
            } else {
                std::print("{}", toml);
            }
        });
    });
}

} // namespace nodehammer::cli::detail

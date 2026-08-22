#include "cli_common.hpp"
#include "run_internal.hpp"

#include <CLI/CLI.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <config/config_writer.hpp>
#include <lua/lua_config.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <sstream>
#include <string>

namespace {

/// Read a whole file, or say which one could not be opened.
std::string slurp(const std::string &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw nodehammer::Error{nodehammer::codes::kFatalCliFileOpen,
                                std::format("could not open '{}' for reading", path), path};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// Whether the config is a script rather than a document.
///
/// The extension, and nothing else. This used to be the difference between two
/// commands, which meant the user had to know it and say it; the file already
/// says it.
bool isLuaConfig(const std::string &path) {
    auto ext = std::filesystem::path{path}.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".lua";
}

/// Load a config from either spelling, already flattened.
///
/// Both arrive at one `NHConfig`: `loadFromFile` resolves a TOML include tree
/// recursively, and `evalLuaConfig` discovers a script's include set by running
/// it. What comes back is the merged form either way, which is why the two had
/// the same output format and the same options and differed only here.
nodehammer::config::ConfigResult loadEitherWay(const std::string &path) {
    if (!isLuaConfig(path)) {
        return nodehammer::config::ConfigLoader::loadFromFile(path);
    }
    // The path the user typed is the root key, so include()/use() resolve
    // against the script's own directory — and a bare name roots at the working
    // directory, which is the application-level choice this command is entitled
    // to make on the user's behalf because they named the path relative to it.
    const std::string rootKey = std::filesystem::path{path}.lexically_normal().generic_string();
    return nodehammer::lua::evalLuaConfig(slurp(path), rootKey,
                                          nodehammer::config::ConfigLoader::filesystemFetcher());
}

/// Write `text` to `outOpt`'s path, or to stdout when it was not given.
void emit(const CLI::Option *outOpt, std::string_view what, const std::string &text) {
    if (!*outOpt) {
        std::print("{}", text);
        return;
    }
    std::string outPath;
    outOpt->results(outPath);
    std::ofstream out{outPath};
    if (!out) {
        throw nodehammer::Error{nodehammer::codes::kFatalCliFileOpen,
                                std::format("could not open '{}' for writing", outPath), outPath};
    }
    out << text;
    std::println(stderr, "{}: wrote {} ({} bytes)", what, outPath, text.size());
}

} // namespace

namespace nodehammer::cli::detail {

void registerCmdConfig(CLI::App &app, const RunOptions &) {
    auto *sub =
        app.add_subcommand("config", "Work with TOML and Lua configs")->require_subcommand(1);

    // ── validate ─────────────────────────────────────────────────────────────
    auto *valSub = sub->add_subcommand("validate", "Check a config and report what is wrong");
    auto *valConfigOpt =
        valSub->add_option("-c,--config", "Config file (.toml / .lua)")->required();

    valSub->callback([=] {
        runOrReport("config validate", [&] {
            std::string configPath;
            valConfigOpt->results(configPath);

            // The collecting face, deliberately: this command promises a report,
            // so a document that will not parse is its answer rather than its
            // failure (docs/error-model.md).
            //
            // Only the TOML loader has one. A script that will not run has no
            // partial config to report against, so the Lua path reports what it
            // observed and stops there.
            if (isLuaConfig(configPath)) {
                auto result = loadEitherWay(configPath);
                printDiags(result.diags);
                if (result.diags.hasErrors()) {
                    std::println(stderr, "config: INVALID (script errors)");
                    return;
                }
                auto validationDiags = config::ConfigValidator::validate(result.config);
                printDiags(validationDiags);
                std::println(validationDiags.hasErrors() ? stderr : stdout, "config: {}",
                             validationDiags.hasErrors() ? "INVALID" : "OK");
                return;
            }

            auto result = config::ConfigLoader::collectFromFile(configPath);
            printDiags(result.diags);
            if (result.diags.hasErrors()) {
                std::println(stderr, "config: INVALID (parse errors)");
                return;
            }

            auto validationDiags = config::ConfigValidator::validate(result.config);
            printDiags(validationDiags);
            if (validationDiags.hasErrors()) {
                std::println(stderr, "config: INVALID");
            } else {
                std::println("config: OK");
            }
        });
    });

    // ── flatten ──────────────────────────────────────────────────────────────
    //
    // One command for both input languages. They were two -- `config-flatten`
    // and `config-lua` -- whose own help text already claimed the same output:
    // flattened TOML, round-trippable through ConfigLoader. Naming the input
    // language in the command meant the user said what the extension already
    // said.
    auto *flatSub = sub->add_subcommand(
        "flatten", "Resolve a config to a single self-contained TOML file. A .lua script is "
                   "evaluated first; its output is round-trippable through ConfigLoader.");
    auto *flatConfigOpt =
        flatSub->add_option("-c,--config", "Input config file (.toml / .lua)")->required();
    auto *flatOutOpt =
        flatSub->add_option("-o,--output", "Output path (defaults to stdout if omitted)");
    auto validate = std::make_shared<bool>(true);
    flatSub->add_flag("!--no-validate", *validate,
                      "Skip ConfigValidator after loading (validation is on by default)");

    flatSub->callback([=] {
        runOrReport("config flatten", [&] {
            std::string configPath;
            flatConfigOpt->results(configPath);

            auto result = loadEitherWay(configPath);
            // The TOML path only reported these; the Lua path treated them as
            // fatal, because a script that failed produced no config to write.
            // Kept as the stricter of the two: a flatten that emitted a document
            // built from a failed load would be worse than one that refused.
            reportOrThrow(result.diags, configPath);

            if (*validate) {
                auto validationDiags = config::ConfigValidator::validate(result.config);
                reportOrThrow(validationDiags, configPath);
            }

            emit(flatOutOpt, "config flatten", config::configToToml(result.config));
        });
    });
}

} // namespace nodehammer::cli::detail

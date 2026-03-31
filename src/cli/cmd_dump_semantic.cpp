#include "nodehammer/ir/diagnostic_codes.hpp"
#include <CLI/CLI.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/selection/selector.hpp>
#include <print>
#include <string>

void register_cmd_dump_semantic(CLI::App &app) {
    auto *sub = app.add_subcommand("dump-semantic", "Dump the semantic IR of a geometry as JSON");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (required when --input is not given)");
    auto *configOpt = sub->add_option("-c,--config", "TOML config file (applies selection)");
    auto *outputOpt = sub->add_option("-o,--output", "Output JSON file (default: stdout)");

    sub->callback([=] {
        std::string inputPath;
        std::string formatStr;
        if (*inputOpt)
            inputOpt->results(inputPath);
        if (*formatOpt)
            formatOpt->results(formatStr);

        if (inputPath.empty() && formatStr.empty()) {
            std::println(stderr, "dump-semantic: provide --input and/or --input-format");
            return;
        }

        // ── Load config (optional) ─────────────────────────────────────────────
        nodehammer::NHConfig cfg;
        if (*configOpt) {
            std::string cfgPath;
            configOpt->results(cfgPath);
            auto loaded = nodehammer::ConfigLoader::loadFromFile(cfgPath);
            for (const auto &d : loaded.diags.items())
                std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                             d.message);
            if (loaded.diags.hasErrors()) {
                std::println(stderr, "dump-semantic: config load failed");
                return;
            }
            cfg = std::move(loaded.config);
            auto validDiags = nodehammer::ConfigValidator::validate(cfg);
            for (const auto &d : validDiags.items())
                std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                             d.message);
            if (validDiags.hasErrors()) {
                std::println(stderr, "dump-semantic: config validation failed");
                return;
            }
        }

        // ── Import ─────────────────────────────────────────────────────────────
        auto registry = nodehammer::makeDefaultRegistry();
        const auto *imp = registry.resolve(inputPath, formatStr);
        if (!imp) {
            std::println(stderr, "[error] {} cannot determine input format{}",
                         nodehammer::codes::kErrImportFormatUnknown,
                         inputPath.empty() ? "" : " for " + inputPath);
            return;
        }

        auto result = imp->import(inputPath);
        for (const auto &d : result.diags.items())
            std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                         d.message);

        // ── Select ─────────────────────────────────────────────────────────────
        if (!cfg.selection.empty()) {
            nodehammer::SelectionEngine sel{cfg.selection, cfg.hoistOrphans};
            auto selDiags = sel.prune(result.scene);
            for (const auto &d : selDiags.items())
                std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                             d.message);
        }

        // ── Dump ───────────────────────────────────────────────────────────────
        nlohmann::json j = result.scene;
        std::string outPath;
        if (*outputOpt) {
            outputOpt->results(outPath);
            std::ofstream f{outPath};
            f << j.dump(2) << '\n';
        } else {
            std::println("{}", j.dump(2));
        }
    });
}

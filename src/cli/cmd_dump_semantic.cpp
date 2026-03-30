#include "nodehammer/ir/diagnostic_codes.hpp"
#include <CLI/CLI.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <print>
#include <string>

void register_cmd_dump_semantic(CLI::App &app) {
    auto *sub = app.add_subcommand("dump-semantic", "Dump the semantic IR of a geometry as JSON");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (required when --input is not given)");
    auto *outputOpt = sub->add_option("-o,--output", "Output JSON file (default: stdout)");

    sub->callback([=] {
        std::string inputPath;
        std::string formatStr;
        if (*inputOpt) {
            inputOpt->results(inputPath);
        }
        if (*formatOpt) {
            formatOpt->results(formatStr);
        }

        if (inputPath.empty() && formatStr.empty()) {
            std::println(stderr, "dump-semantic: provide --input and/or --input-format");
            return;
        }

        auto registry = nodehammer::makeDefaultRegistry();
        const auto *imp = registry.resolve(inputPath, formatStr);
        if (!imp) {
            std::println(stderr, "[error] {} cannot determine input format{}",
                         nodehammer::codes::kErrImportFormatUnknown,
                         inputPath.empty() ? "" : " for " + inputPath);
            return;
        }

        auto result = imp->import(inputPath);
        for (const auto &d : result.diags.items()) {
            std::println(stderr, "[{}] {} {}", nodehammer::severityName(d.severity), d.code,
                         d.message);
        }

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

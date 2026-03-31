#include <CLI/CLI.hpp>
#include <map>
#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <print>
#include <string>

void register_cmd_inspect(CLI::App &app) {
    auto *sub = app.add_subcommand("inspect", "Inspect a geometry file and print a summary");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (required when --input is not given)");

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
            std::println(stderr, "inspect: provide --input and/or --input-format");
            return;
        }

        auto registry = nodehammer::makeDefaultRegistry();
        const auto *imp = registry.resolve(inputPath, formatStr);
        if (!imp) {
            std::println(stderr, "[error] NH0101 cannot determine input format{}",
                         inputPath.empty() ? "" : " for " + inputPath);
            return;
        }

        auto result = imp->import(inputPath);

        // Shape histogram via std::visit
        std::map<std::string, int> shapeCounts;
        for (const auto &[id, shape] : result.scene.shapes) {
            std::string typeName = std::visit(
                nodehammer::detail::overloaded{
                    [](const nodehammer::BoxShape &) -> std::string { return "box"; },
                    [](const nodehammer::TubeShape &) -> std::string { return "tube"; },
                    [](const nodehammer::ConeShape &) -> std::string { return "cone"; },
                    [](const nodehammer::TrdShape &) -> std::string { return "trd"; },
                    [](const nodehammer::ParaShape &) -> std::string { return "para"; },
                    [](const nodehammer::PconShape &) -> std::string { return "pcon"; },
                    [](const nodehammer::PgonShape &) -> std::string { return "pgon"; },
                    [](const nodehammer::TorusShape &) -> std::string { return "torus"; },
                    [](const nodehammer::TessellatedShape &) -> std::string {
                        return "tessellated";
                    },
                    [](const nodehammer::BooleanUnion &) -> std::string { return "union"; },
                    [](const nodehammer::BooleanIntersection &) -> std::string {
                        return "intersection";
                    },
                    [](const nodehammer::BooleanSubtraction &) -> std::string {
                        return "subtraction";
                    },
                    [](const nodehammer::UnknownShape &) -> std::string { return "unknown"; },
                },
                shape.data);
            shapeCounts[typeName]++;
        }

        // Material names
        std::vector<std::string> matNames;
        matNames.reserve(result.scene.materials.size());
        for (const auto &[id, mat] : result.scene.materials) {
            matNames.push_back(mat.name);
        }

        int warnings = 0;
        int errors = 0;
        for (const auto &d : result.diags.items()) {
            if (d.severity == nodehammer::DiagnosticSeverity::Warning) {
                ++warnings;
            }
            if (d.severity >= nodehammer::DiagnosticSeverity::Error) {
                ++errors;
            }
        }

        std::println("Format:    {}", imp->formatName());
        std::println("Nodes:     {}", result.scene.nodes.size());

        std::string shapeStr;
        for (const auto &[name, count] : shapeCounts) {
            if (!shapeStr.empty()) {
                shapeStr += ", ";
            }
            shapeStr += std::format("{}={}", name, count);
        }
        std::println("Shapes:    {}", shapeStr.empty() ? "(none)" : shapeStr);

        std::string matStr;
        for (const auto &n : matNames) {
            if (!matStr.empty()) {
                matStr += ", ";
            }
            matStr += n;
        }
        std::println("Materials ({}): {}", matNames.size(), matStr.empty() ? "(none)" : matStr);
        std::println("Warnings: {}  Errors: {}", warnings, errors);
    });
}

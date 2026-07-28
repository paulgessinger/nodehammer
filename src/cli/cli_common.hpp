#pragma once

#include <CLI/CLI.hpp>
#include <nodehammer/detail/markup.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic/importer.hpp>

#include <nodehammer/ir/diagnostics.hpp>

#include <print>
#include <string>

namespace nodehammer::cli {

/// Print diagnostics to stderr with colored severity.
inline void printDiags(const DiagnosticList &diags) {
    static detail::Console errCon{detail::ColorMode::Auto};
    for (const auto &d : diags.items()) {
        std::string_view color;
        std::string_view label;
        if (d.severity >= DiagnosticSeverity::Error) {
            color = "red";
            label = "error";
        } else if (d.severity == DiagnosticSeverity::Warning) {
            color = "yellow";
            label = "warn";
        } else {
            color = "dim";
            label = "info";
        }
        errCon.println(stderr, "[{}][bold]{} {}[/]  {}", color, label, d.code, d.message);
    }
}

/// Result of importOrExit: the import result plus the format name.
struct ImportWithFormat {
    detail::ImportResult result;
    std::string formatName;
};

/// Read --input and --input-format from CLI options, import the file,
/// print errors, and exit on failure.
inline ImportWithFormat importOrExit(CLI::Option *inputOpt, CLI::Option *formatOpt) {
    std::string inputPath, inputFmt;
    if (*inputOpt) {
        inputOpt->results(inputPath);
    }
    if (formatOpt != nullptr && *formatOpt) {
        formatOpt->results(inputFmt);
    }
    if (inputPath.empty()) {
        std::println(stderr, "error: --input is required");
        std::exit(1);
    }

    auto registry = ImporterRegistry::makeDefault();
    const auto *imp = registry.resolve(inputPath, inputFmt);
    if (imp == nullptr) {
        std::println(stderr, "[error] {} cannot determine input format for '{}'",
                     codes::kErrImportFormatUnknown, inputPath);
        std::exit(1);
    }
    auto result = imp->import(inputPath);
    return {std::move(result), std::string{imp->formatName()}};
}

} // namespace nodehammer::cli

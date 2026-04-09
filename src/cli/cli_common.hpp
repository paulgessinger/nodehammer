#pragma once

#include <CLI/CLI.hpp>
#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/markup.hpp>

#include <print>
#include <string>

namespace nodehammer::cli {

/// Result of importOrExit: the import result plus the format name.
struct ImportWithFormat {
    ImportResult result;
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

    auto registry = makeDefaultRegistry();
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

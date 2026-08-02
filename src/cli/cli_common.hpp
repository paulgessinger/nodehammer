#pragma once

#include <CLI/CLI.hpp>
#include <detail/markup.hpp>
#include <diagnostic_codes.hpp>
#include <ir/semantic/importer.hpp>

#include <diagnostics.hpp>

#include <format>
#include <print>
#include <string>
#include <utility>

namespace nodehammer::cli {

/// Print one diagnostic to stderr with coloured severity.
inline void printDiag(const Diagnostic &d) {
    static detail::Console errCon{detail::ColorMode::Auto};
    std::string_view color;
    std::string_view label;
    if (d.severity == diagnostics::Severity::Fatal) {
        color = "red";
        label = "fatal";
    } else if (d.severity == diagnostics::Severity::Error) {
        color = "red";
        label = "error";
    } else if (d.severity == diagnostics::Severity::Warning) {
        color = "yellow";
        label = "warn";
    } else {
        color = "dim";
        label = "info";
    }
    errCon.println(stderr, "[{}][bold]{} {}[/]  {}", color, label, d.code, d.message);
}

inline void printDiags(const diagnostics::List &diags) {
    for (const auto &d : diags.items()) {
        printDiag(d);
    }
}

/// The public list, which is what an `Error` carries.
inline void printDiags(const DiagnosticList &diags) {
    for (const auto &d : diags) {
        printDiag(d);
    }
}

/// Run a command body, and turn a named failure into the one exit path a CLI
/// has.
///
/// This is what replaces the per-stage `if (diags.hasErrors()) exit(1)` ladder
/// every command used to carry: under docs/error-model.md a stage that could
/// not deliver throws, so there is exactly one place per command that has to
/// know what to do about it.
///
/// `Error::observed()` is printed first — for a failure that came from a
/// collector it holds every problem found, not just the one the exception
/// names. When it is empty the failure had nothing to collect, so the fatal
/// diagnostic itself is printed instead.
template <typename Body> void runOrExit(std::string_view command, Body &&body) {
    try {
        std::forward<Body>(body)();
        return;
    } catch (const Error &e) {
        if (e.observed().empty()) {
            printDiag(e.diagnostic());
        } else {
            printDiags(e.observed());
        }
        std::println(stderr, "{}: {}", command, e.what());
    }
    std::exit(1);
}

/// Result of importFrom: the import result plus the format name.
struct ImportWithFormat {
    ir::ImportResult result;
    std::string formatName;
};

/// Read --input and --input-format from CLI options and import the file.
///
/// Throws rather than exiting: `runOrExit` is where a command ends, and a
/// helper that called `exit` itself would be a second one.
inline ImportWithFormat importFrom(CLI::Option *inputOpt, CLI::Option *formatOpt) {
    std::string inputPath, inputFmt;
    if (*inputOpt) {
        inputOpt->results(inputPath);
    }
    if (formatOpt != nullptr && *formatOpt) {
        formatOpt->results(inputFmt);
    }
    if (inputPath.empty()) {
        throw Error{codes::kFatalImportFormatUnknown, "--input is required"};
    }

    auto registry = ir::ImporterRegistry::makeDefault();
    const auto *imp = registry.resolve(inputPath, inputFmt);
    if (imp == nullptr) {
        throw Error{codes::kFatalImportFormatUnknown,
                    std::format("cannot determine input format for '{}'", inputPath), inputPath};
    }
    auto result = imp->import(inputPath);
    return {std::move(result), std::string{imp->formatName()}};
}

} // namespace nodehammer::cli

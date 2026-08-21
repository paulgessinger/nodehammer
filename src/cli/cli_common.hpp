#pragma once

#include "run_internal.hpp"

#include <CLI/CLI.hpp>
#include <detail/markup.hpp>
#include <diagnostic_codes.hpp>
#include <ir/semantic/importer.hpp>

#include <diagnostics.hpp>

#include <format>
#include <print>
#include <span>
#include <string>
#include <utility>

namespace nodehammer::cli {

/// Print one diagnostic to stderr with coloured severity.
inline void printDiag(const Diagnostic &d) {
    // Deliberately not `static`. A `Console` holds a colour *mode*, not a
    // decision — `shouldColorize` re-checks the descriptor on every call — so
    // the static cached nothing, and process-lifetime state in a library that
    // can now be called twice is a hazard for no gain.
    const nodehammer::detail::Console errCon{nodehammer::detail::ColorMode::Auto};
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

/// One overload, taking a borrowed range: a `DiagnosticList` and the span an
/// `Error` carries are the same sequence, and used to need two functions only
/// because they were two types.
inline void printDiags(std::span<const Diagnostic> diags) {
    for (const auto &d : diags) {
        printDiag(d);
    }
}

/// Print what a stage collected, then fail if it collected an error.
///
/// The two halves are deliberately exclusive. `throwIfErrors` puts the whole
/// list on the exception and `runOrReport` prints that list, so printing here
/// as well would say everything twice — which is what the hand-written
/// `printDiags(...); if (hasErrors()) { println("..."); exit(1); }` ladders
/// avoided only by not throwing at all.
inline void reportOrThrow(const DiagnosticList &diags, std::string_view context) {
    if (!diags.hasErrors()) {
        printDiags(diags);
        return;
    }
    diagnostics::throwIfErrors(diags, context);
}

/// Run a command body, and turn a named failure into an exit code.
///
/// This is what replaces the per-stage `if (diags.hasErrors()) exit(1)` ladder
/// every command used to carry: under docs/error-model.md a stage that could
/// not deliver throws, so there is exactly one place per command that has to
/// know what to do about it.
///
/// It reports, then throws `CommandFailure` — it does *not* terminate. That
/// distinction is the whole point of this pass: these command bodies now compile
/// into the shared library, where `std::exit` would end the caller's process,
/// which for a Python caller means the interpreter, with no traceback and no
/// `finally`.
///
/// `Error::observed()` is printed first — for a failure that came from a
/// collector it holds every problem found, not just the one the exception
/// names. When it is empty the failure had nothing to collect, so the fatal
/// diagnostic itself is printed instead.
template <typename Body> void runOrReport(std::string_view command, Body &&body) {
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
    throw detail::CommandFailure{1};
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

#pragma once

namespace nodehammer::cli {

/// The entire CLI: builds the CLI11 app, registers every subcommand, parses
/// `argv`, and dispatches. Returns the process exit code.
///
/// This is deliberately *not* `main`. The native `nodehammer` executable is a
/// three-line shim over it (`src/cli/main.cpp`), and the Python extension
/// module calls it directly (`src/python/bindings.cpp`) so a wheel ships one
/// native artifact instead of a binary plus a near-identical shared library.
/// Keeping the body here rather than behind a build-mode `#ifdef` means both
/// entry points run byte-identical code.
///
/// `argv` must be NUL-terminated C strings and `argv[argc]` must be `nullptr`,
/// matching the `main` convention — CLI11 reads `argv[0]` for usage text.
int run(int argc, char **argv);

} // namespace nodehammer::cli

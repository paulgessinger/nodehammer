#pragma once

// The CLI's internals — what `include/nodehammer/cli.hpp` deliberately does not
// say.
//
// The public header carries one function and a struct of options, because that
// is all an installed consumer can be given: everything else here names
// `CLI::App`, which is CLI11's type, not ours to publish.
//
// The namespace is spelled in the joined form on purpose. `ci/check_shared_exports.py`
// harvests `^namespace nodehammer::x::y` to decide what is internal, and a
// nested `namespace nodehammer::cli { namespace detail {` would register only as
// `nodehammer::cli` — which is *public*, since a public header declares it. The
// joined spelling is what keeps this half hidden in the checker's eyes as well
// as the linker's.

#include <string_view>

namespace nodehammer::cli::detail {

/// The exit code, on its way out of a subcommand callback.
///
/// CLI11 callbacks return `void`, so a command that has decided on a non-zero
/// code has no way to hand it back. Throwing is that way: the entry point
/// catches this and returns `code`.
///
/// Deliberately derived from nothing. CLI11 catches `CLI::Error`,
/// `CLI::ValidationError`, `CLI::ConversionError`, `CLI::ArgumentMismatch`,
/// `CLI::FileError` and `std::invalid_argument` at various points inside
/// `App::parse`; a type in none of those hierarchies is the one shape that
/// provably passes through all of them untouched.
///
/// It is not an error type and carries no message — by the time it is thrown the
/// failure has already been reported. `nodehammer::Error` remains the exception
/// that *describes* a failure; this one only carries the number.
struct CommandFailure {
    int code = 1;
};

} // namespace nodehammer::cli::detail

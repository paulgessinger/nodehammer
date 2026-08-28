#pragma once

// Reading an environment variable, without the three ways `std::getenv` goes
// wrong on Windows.
//
// MSVC deprecates it (C4996) and NODEHAMMER_WERROR promotes that to an error,
// so a plain call there is not merely unidiomatic — it does not compile.
// GetEnvironmentVariableA also sizes the buffer correctly and is thread-safe
// against a concurrent SetEnvironmentVariable, which the pointer `getenv`
// returns is not.
//
// This started as one copy in the pager, was copied to the web runtime locator,
// and was copied a third time by `skills` — which is when the Windows leg
// caught it, because that third copy used `std::getenv` directly.
//
// The definition lives in env.cpp so <windows.h> stays in a single translation
// unit rather than reaching every includer along with its macros.

#include <string>

namespace nodehammer::detail {

/// The value of environment variable `name`.
///
/// Empty when the variable is unset *or* set to the empty string. The two are
/// deliberately not distinguished: every caller treats "" as absent.
std::string getEnv(const char *name);

} // namespace nodehammer::detail

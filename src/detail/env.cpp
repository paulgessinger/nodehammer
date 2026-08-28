#include <detail/env.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace nodehammer::detail {

#ifdef _WIN32

std::string getEnv(const char *name) {
    // The first call sizes the buffer and counts the terminator; the second
    // returns the length without it, so `written >= needed` means the variable
    // changed underneath us and the result is not trustworthy.
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string buf(needed, '\0');
    DWORD written = GetEnvironmentVariableA(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
}

#else

std::string getEnv(const char *name) {
    const char *val = std::getenv(name);
    return (val != nullptr && val[0] != '\0') ? val : std::string{};
}

#endif

} // namespace nodehammer::detail

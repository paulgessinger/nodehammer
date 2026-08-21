#include "web/runtime_locator.hpp"

// Where am I? One question, four answers, and `argv[0]` is none of them — it is
// whatever the caller chose to put there, and for a library reached from Python
// it is not even the right process's idea of the program.
//
// Split out from runtime_locator.cpp because this is the only platform-
// conditional code in the pair, and mixing the two would put three #if branches
// in the middle of the ladder.

#include <system_error>
#include <vector>

#if defined(__EMSCRIPTEN__)
// Nothing. A wasm module has no executable path, and the ladder treats that as
// one rung unavailable rather than as a failure.
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#else
// Linux and the other /proc systems: read the symlink.
#endif

namespace nodehammer::web {

std::optional<std::filesystem::path> executablePath() {
#if defined(__EMSCRIPTEN__)
    return std::nullopt;

#elif defined(_WIN32)
    // GetModuleFileNameW truncates rather than failing when the buffer is too
    // small, and reports the truncated length, so growing on "filled exactly"
    // is the documented way to know.
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return std::nullopt;
        }
        if (n < buf.size()) {
            return std::filesystem::path{std::wstring{buf.data(), n}};
        }
        buf.resize(buf.size() * 2);
    }

#elif defined(__APPLE__)
    // Called twice on purpose: the first call reports the size it wants, and
    // the path it returns is not canonical (it can carry .. and symlinks), so
    // weakly_canonical below is not optional here.
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return std::nullopt;
    }
    std::error_code ec;
    std::filesystem::path p =
        std::filesystem::weakly_canonical(std::filesystem::path{buf.data()}, ec);
    if (ec) {
        return std::filesystem::path{buf.data()};
    }
    return p;

#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::nullopt;
    }
    return p;
#endif
}

} // namespace nodehammer::web

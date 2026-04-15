#include "pager.hpp"

#include <cstdlib>
#include <format>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace nodehammer::cli {

namespace {

#ifdef _WIN32
/// Thread-safe environment variable lookup on Windows.
/// Returns empty string if the variable is not set or empty.
std::string getEnv(const char *name) {
    // Use GetEnvironmentVariableA: no deprecation warnings, correct size
    // handling, and thread-safe on Windows.
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

/// Returns true if `exe` can be found on the system PATH.
bool exeOnPath([[maybe_unused]] const char *exe) {
#ifdef _WIN32
    // "where" exits 0 when found, non-zero otherwise.
    auto cmd = std::format("where {} >nul 2>nul", exe);
    return std::system(cmd.c_str()) == 0;
#else
    auto cmd = std::format("command -v {} >/dev/null 2>&1", exe);
    return std::system(cmd.c_str()) == 0;
#endif
}

struct PagerChoice {
    std::string command;
    bool supportsAnsi;
};

/// Pick the best available pager command.
PagerChoice choosePager() {
    // 1. Honour the user's explicit choice.
    std::string env = getEnv("PAGER");
    if (!env.empty()) {
        // Heuristic: if the pager name contains "less" or "bat", assume ANSI
        // support.  For anything else, be conservative.
        bool ansi = env.find("less") != std::string::npos ||
                    env.find("bat") != std::string::npos;
        return {env, ansi};
    }

    // 2. Platform defaults.
#ifdef _WIN32
    // Git for Windows puts less.exe on PATH; prefer it when available.
    if (exeOnPath("less")) {
        return {"less -R", true};
    }
    // "more" ships with every Windows install.  It lacks ANSI escape support
    // and scroll-back, but still provides basic page-by-page viewing.
    // effectiveColorMode() will return ColorMode::Never so output won't be
    // garbled by raw escape sequences.
    return {"more", false};
#else
    return {"less -R", true};
#endif
}

} // namespace

Pager::Pager() {
    if (!nodehammer::detail::Console::isTTY()) {
        return;
    }

    auto [cmd, ansi] = choosePager();

    // Save original stdout fd.
    savedFd_ = duplicateFd(fileNo(stdout));
    if (savedFd_ < 0) {
        return;
    }

    pipe_ = popenPipe(cmd.c_str(), "w");
    if (pipe_ == nullptr) {
        closeFd(savedFd_);
        savedFd_ = -1;
        return;
    }

    // Redirect stdout fd to the pipe.
    std::fflush(stdout);
    duplicateTo(fileNo(pipe_), fileNo(stdout));
    active_ = true;
    pagerSupportsAnsi_ = ansi;
}

Pager::~Pager() {
    if (!active_) {
        return;
    }

    // Restore original stdout.
    std::fflush(stdout);
    duplicateTo(savedFd_, fileNo(stdout));
    closeFd(savedFd_);
    pclosePipe(pipe_);
}

bool Pager::isActive() const { return active_; }

detail::ColorMode Pager::effectiveColorMode(detail::ColorMode requested) const {
    if (active_ && requested == detail::ColorMode::Auto) {
        return pagerSupportsAnsi_ ? detail::ColorMode::Always
                                  : detail::ColorMode::Never;
    }
    return requested;
}

int Pager::fileNo(FILE *f) {
#ifdef _WIN32
    return _fileno(f);
#else
    return fileno(f);
#endif
}

int Pager::duplicateFd(int fd) {
#ifdef _WIN32
    return _dup(fd);
#else
    return dup(fd);
#endif
}

int Pager::duplicateTo(int source, int target) {
#ifdef _WIN32
    return _dup2(source, target);
#else
    return dup2(source, target);
#endif
}

int Pager::closeFd(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

FILE *Pager::popenPipe(const char *command, const char *mode) {
#ifdef _WIN32
    return _popen(command, mode);
#else
    return popen(command, mode);
#endif
}

int Pager::pclosePipe(FILE *pipe) {
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

} // namespace nodehammer::cli

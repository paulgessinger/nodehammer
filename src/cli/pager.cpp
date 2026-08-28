#include "pager.hpp"

#include <detail/env.hpp>

#include <cstdlib>
#include <format>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nodehammer::cli {

namespace {

#ifdef _WIN32
/// Returns true if `exe` can be found on the system PATH.
bool exeOnPath(const char *exe) {
    // "where" exits 0 when found, non-zero otherwise.
    auto cmd = std::format("where {} >nul 2>nul", exe);
    return std::system(cmd.c_str()) == 0;
}
#endif

struct PagerChoice {
    std::string command;
    bool supportsAnsi;
};

/// Pick the best available pager command.
PagerChoice choosePager() {
    // 1. Honour the user's explicit choice.
    std::string env = nodehammer::detail::getEnv("PAGER");
    if (!env.empty()) {
        // Heuristic: if the pager name contains "less" or "bat", assume ANSI
        // support.  For anything else, be conservative.
        bool ansi = env.find("less") != std::string::npos || env.find("bat") != std::string::npos;
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

Pager::Pager(bool enabled) {
    if (!enabled || !nodehammer::detail::Console::isTTY()) {
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

nodehammer::detail::ColorMode
Pager::effectiveColorMode(nodehammer::detail::ColorMode requested) const {
    if (active_ && requested == nodehammer::detail::ColorMode::Auto) {
        return pagerSupportsAnsi_ ? nodehammer::detail::ColorMode::Always
                                  : nodehammer::detail::ColorMode::Never;
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

#pragma once

#include "nodehammer/markup.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace nodehammer::cli {

/// RAII pager: on construction, if stdout is a TTY, opens a pipe to a pager
/// (less -R, or $PAGER) and replaces the stdout file descriptor with the pipe.
/// On destruction, restores stdout and waits for the pager to exit.
/// If stdout is not a TTY (piped/redirected), does nothing.
class Pager {
  public:
    Pager() {
        if (!nodehammer::Console::isTTY()) {
            return;
        }

        const char *pager = std::getenv("PAGER");
        std::string cmd = (pager != nullptr && pager[0] != '\0') ? pager : "less -R";

        // Save original stdout fd.
        savedFd_ = dup(fileno(stdout));
        if (savedFd_ < 0) {
            return;
        }

        pipe_ = popen(cmd.c_str(), "w");
        if (pipe_ == nullptr) {
            close(savedFd_);
            savedFd_ = -1;
            return;
        }

        // Redirect stdout fd to the pipe.
        std::fflush(stdout);
        dup2(fileno(pipe_), fileno(stdout));
        active_ = true;
    }

    ~Pager() {
        if (!active_) {
            return;
        }
        // Restore original stdout.
        std::fflush(stdout);
        dup2(savedFd_, fileno(stdout));
        close(savedFd_);
        pclose(pipe_);
    }

    /// True when output is going through a pager.
    [[nodiscard]] bool isActive() const { return active_; }

    /// Returns the color mode to use: Always when paging (pager handles ANSI),
    /// otherwise the requested mode.
    [[nodiscard]] ColorMode effectiveColorMode(ColorMode requested = ColorMode::Auto) const {
        if (active_ && requested == ColorMode::Auto) {
            return ColorMode::Always;
        }
        return requested;
    }

    Pager(const Pager &) = delete;
    Pager &operator=(const Pager &) = delete;

  private:
    FILE *pipe_{nullptr};
    int savedFd_{-1};
    bool active_{false};
};

} // namespace nodehammer::cli

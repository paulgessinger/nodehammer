#pragma once

#include <cstdio>
#include <detail/markup.hpp>

namespace nodehammer::cli {

// `nodehammer::detail::` is spelled in full throughout this header and its
// implementation: `nodehammer::cli::detail` (run_internal.hpp) is the nearer
// enclosing scope, so a bare `detail::` resolves there and does not fall back.

/// RAII pager: on construction, if paging is enabled and stdout is a TTY, opens
/// a pipe to a pager (less -R, or $PAGER) and replaces the stdout file
/// descriptor with the pipe. On destruction, restores stdout and waits for the
/// pager to exit. If stdout is not a TTY (piped/redirected), does nothing.
///
/// `enabled` is not a convenience. A TTY is the *executable's* evidence that a
/// human is reading; for `cli::run` called as an API it is evidence of nothing —
/// an interactive interpreter has a TTY too, and paging there replaces the
/// caller's fd 1 with a pipe and then blocks in `pclose` until somebody quits
/// `less`. So the caller says, and `RunOptions::pager` is where they say it.
class Pager {
  public:
    explicit Pager(bool enabled);
    ~Pager();

    /// True when output is going through a pager.
    [[nodiscard]] bool isActive() const;

    /// Returns the color mode to use: Always when paging (pager handles ANSI),
    /// otherwise the requested mode.
    [[nodiscard]] nodehammer::detail::ColorMode effectiveColorMode(
        nodehammer::detail::ColorMode requested = nodehammer::detail::ColorMode::Auto) const;

    Pager(const Pager &) = delete;
    Pager &operator=(const Pager &) = delete;

  private:
    static int fileNo(FILE *f);
    static int duplicateFd(int fd);
    static int duplicateTo(int source, int target);
    static int closeFd(int fd);
    static FILE *popenPipe(const char *command, const char *mode);
    static int pclosePipe(FILE *pipe);

    FILE *pipe_{nullptr};
    int savedFd_{-1};
    bool active_{false};
    bool pagerSupportsAnsi_{false};
};

} // namespace nodehammer::cli

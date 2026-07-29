#pragma once

#include <cstdio>
#include <detail/markup.hpp>

namespace nodehammer::cli {

/// RAII pager: on construction, if stdout is a TTY, opens a pipe to a pager
/// (less -R, or $PAGER) and replaces the stdout file descriptor with the pipe.
/// On destruction, restores stdout and waits for the pager to exit.
/// If stdout is not a TTY (piped/redirected), does nothing.
class Pager {
  public:
    Pager();
    ~Pager();

    /// True when output is going through a pager.
    [[nodiscard]] bool isActive() const;

    /// Returns the color mode to use: Always when paging (pager handles ANSI),
    /// otherwise the requested mode.
    [[nodiscard]] detail::ColorMode
    effectiveColorMode(detail::ColorMode requested = detail::ColorMode::Auto) const;

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

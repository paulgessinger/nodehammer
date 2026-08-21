#include "web/open_browser.hpp"

#if defined(_WIN32)
// windows.h first, and in its own block so the sort keeps it there: shellapi.h
// is not self-contained, and without the base types it fails to parse.
#include <windows.h>

#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace nodehammer::web {

#if defined(_WIN32)

bool openInBrowser(const std::string &url) {
    // ShellExecute takes the URL as one argument and does no shell parsing of
    // it, so there is nothing here to quote or escape.
    const auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32; // ShellExecute's documented success threshold
}

#else

namespace {

/// Spawn `argv` detached, without a shell.
///
/// posix_spawn rather than std::system: the latter runs the string through
/// /bin/sh, which would make the URL a place where shell metacharacters mean
/// something. Ours is one we built, but a helper that is only safe for its
/// current caller is a trap for the next one.
bool spawnDetached(const char *program, const std::string &url) {
    const char *argv[] = {program, url.c_str(), nullptr};
    pid_t pid = 0;
    if (::posix_spawnp(&pid, program, nullptr, nullptr, const_cast<char *const *>(argv), environ) !=
        0) {
        return false;
    }
    // Reaped rather than left behind: `open` and `xdg-open` return immediately
    // after handing off, so this does not wait for the browser -- it waits for
    // the launcher, and skipping it would leave a zombie for the life of a
    // long-running serve.
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

bool openInBrowser(const std::string &url) {
#if defined(__APPLE__)
    return spawnDetached("open", url);
#else
    // xdg-open is the freedesktop entry point; every desktop environment ships
    // one, and a box without it is a box with no browser to open.
    return spawnDetached("xdg-open", url);
#endif
}

#endif

} // namespace nodehammer::web

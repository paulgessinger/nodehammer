#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

namespace nodehammer {

/// Parse BB-style markup tags and produce ANSI-colored output.
///
/// Supported tags:
///   Colors:  [black] [red] [green] [yellow] [blue] [magenta] [cyan] [white]
///   Bright:  [bright_red] [bright_green] ... (all eight)
///   Styles:  [bold] [dim] [italic] [underline] [strikethrough]
///   Close:   [/red] [/bold] ... or [/] to reset everything
///   Escape:  \\[ produces a literal '['
std::string markup(std::string_view input);

/// Strip all markup tags, returning plain text.
std::string strip_markup(std::string_view input);

/// Format with std::format, then apply markup.
template <typename... Args>
std::string markup_format(std::format_string<Args...> fmt, Args &&...args) {
    return markup(std::format(fmt, std::forward<Args>(args)...));
}

/// Format with markup, print to stdout with newline.
/// Strips ANSI codes when stdout is not a TTY.
template <typename... Args> void markup_println(std::format_string<Args...> fmt, Args &&...args) {
    auto text = std::format(fmt, std::forward<Args>(args)...);
    if (isatty(STDOUT_FILENO)) {
        std::println("{}", markup(text));
    } else {
        std::println("{}", strip_markup(text));
    }
}

/// Format with markup, print to a FILE* with newline.
/// Strips ANSI codes when the file is not a TTY.
template <typename... Args>
void markup_println(FILE *f, std::format_string<Args...> fmt, Args &&...args) {
    auto text = std::format(fmt, std::forward<Args>(args)...);
    if (isatty(fileno(f))) {
        std::println(f, "{}", markup(text));
    } else {
        std::println(f, "{}", strip_markup(text));
    }
}

} // namespace nodehammer

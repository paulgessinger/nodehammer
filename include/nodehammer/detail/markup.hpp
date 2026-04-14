#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nodehammer::detail {

/// Controls when Console emits ANSI escape codes.
enum class ColorMode {
    Auto,   ///< Emit ANSI only when the output is a TTY (default)
    Always, ///< Always emit ANSI codes
    Never,  ///< Never emit ANSI codes, always strip tags
};

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
std::string stripMarkup(std::string_view input);

/// A console that applies BB-style markup with configurable color mode.
class Console {
  public:
    explicit Console(ColorMode mode = ColorMode::Auto) : m_mode(mode) {}

    void setColorMode(ColorMode mode) { m_mode = mode; }
    ColorMode colorMode() const { return m_mode; }

    /// Format with std::format, then apply markup (always produces ANSI
    /// when color mode is Always/Auto, strips when Never).
    template <typename... Args>
    std::string format(std::format_string<Args...> fmt, Args &&...args) const {
        auto text = std::format(fmt, std::forward<Args>(args)...);
        if (m_mode == ColorMode::Never) {
            return stripMarkup(text);
        }
        return markup(text);
    }

    /// Format with markup, print to stdout with newline.
    template <typename... Args>
    void println(std::format_string<Args...> fmt, Args &&...args) const {
        auto text = std::format(fmt, std::forward<Args>(args)...);
        if (shouldColorize(STDOUT_FILENO)) {
            std::println("{}", markup(text));
        } else {
            std::println("{}", stripMarkup(text));
        }
    }

    /// Format with markup, print to a FILE* with newline.
    template <typename... Args>
    void println(FILE *f, std::format_string<Args...> fmt, Args &&...args) const {
        auto text = std::format(fmt, std::forward<Args>(args)...);
        if (shouldColorize(fileno(f))) {
            std::println(f, "{}", markup(text));
        } else {
            std::println(f, "{}", stripMarkup(text));
        }
    }

    /// Check whether a FILE* is connected to a terminal.
    static bool isTTY(FILE *f = stdout) {
#ifdef _WIN32
        return _isatty(_fileno(f)) != 0;
#else
        return isatty(fileno(f)) != 0;
#endif
    }

  private:
    bool shouldColorize(int fd) const {
        switch (m_mode) {
        case ColorMode::Always:
            return true;
        case ColorMode::Never:
            return false;
        case ColorMode::Auto:
        default:
            return isatty(fd) != 0;
        }
    }

    ColorMode m_mode;
};

/// Format with std::format, then apply markup.
template <typename... Args>
std::string markupFormat(std::format_string<Args...> fmt, Args &&...args) {
    return markup(std::format(fmt, std::forward<Args>(args)...));
}

/// Format with markup, print to stdout with newline (Auto color mode).
template <typename... Args> void markupPrintln(std::format_string<Args...> fmt, Args &&...args) {
    Console{}.println(fmt, std::forward<Args>(args)...);
}

/// Format with markup, print to a FILE* with newline (Auto color mode).
template <typename... Args>
void markupPrintln(FILE *f, std::format_string<Args...> fmt, Args &&...args) {
    Console{}.println(f, fmt, std::forward<Args>(args)...);
}

} // namespace nodehammer::detail

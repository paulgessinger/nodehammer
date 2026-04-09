#include <nodehammer/markup.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace nodehammer {

namespace {

struct AnsiCode {
    std::string_view tag;
    std::string_view open;
    std::string_view close;
};

// Style codes
constexpr std::array<AnsiCode, 5> kStyles = {{
    {"bold", "\033[1m", "\033[22m"},
    {"dim", "\033[2m", "\033[22m"},
    {"italic", "\033[3m", "\033[23m"},
    {"underline", "\033[4m", "\033[24m"},
    {"strikethrough", "\033[9m", "\033[29m"},
}};

// Standard foreground colors (30-37)
constexpr std::array<AnsiCode, 8> kColors = {{
    {"black", "\033[30m", "\033[39m"},
    {"red", "\033[31m", "\033[39m"},
    {"green", "\033[32m", "\033[39m"},
    {"yellow", "\033[33m", "\033[39m"},
    {"blue", "\033[34m", "\033[39m"},
    {"magenta", "\033[35m", "\033[39m"},
    {"cyan", "\033[36m", "\033[39m"},
    {"white", "\033[37m", "\033[39m"},
}};

// Bright foreground colors (90-97)
constexpr std::array<AnsiCode, 8> kBrightColors = {{
    {"bright_black", "\033[90m", "\033[39m"},
    {"bright_red", "\033[91m", "\033[39m"},
    {"bright_green", "\033[92m", "\033[39m"},
    {"bright_yellow", "\033[93m", "\033[39m"},
    {"bright_blue", "\033[94m", "\033[39m"},
    {"bright_magenta", "\033[95m", "\033[39m"},
    {"bright_cyan", "\033[96m", "\033[39m"},
    {"bright_white", "\033[97m", "\033[39m"},
}};

const AnsiCode *findCode(std::string_view tag) {
    for (const auto &c : kStyles) {
        if (c.tag == tag) {
            return &c;
        }
    }
    for (const auto &c : kColors) {
        if (c.tag == tag) {
            return &c;
        }
    }
    for (const auto &c : kBrightColors) {
        if (c.tag == tag) {
            return &c;
        }
    }
    return nullptr;
}

constexpr std::string_view kReset = "\033[0m";

// Parse a [...] tag at position i (pointing at '['). Returns the position
// after the closing ']', or std::string_view::npos if not a valid tag.
// On success, writes the ANSI sequence to `out`.
std::size_t parseTag(std::string_view input, std::size_t i, std::string &out, bool strip) {
    auto close = input.find(']', i);
    if (close == std::string_view::npos) {
        return std::string_view::npos;
    }

    auto tag = input.substr(i + 1, close - i - 1);

    if (strip) {
        // Check that the tag is valid before consuming it
        if (tag == "/") {
            return close + 1;
        }
        if (tag.starts_with('/')) {
            auto name = tag.substr(1);
            if (findCode(name) != nullptr) {
                return close + 1;
            }
        } else {
            if (findCode(tag) != nullptr) {
                return close + 1;
            }
        }
        return std::string_view::npos;
    }

    // Reset-all tag: [/]
    if (tag == "/") {
        out += kReset;
        return close + 1;
    }

    // Closing tag: [/name]
    if (tag.starts_with('/')) {
        auto name = tag.substr(1);
        const auto *code = findCode(name);
        if (code != nullptr) {
            out += code->close;
            return close + 1;
        }
        return std::string_view::npos;
    }

    // Opening tag: [name]
    const auto *code = findCode(tag);
    if (code != nullptr) {
        out += code->open;
        return close + 1;
    }

    return std::string_view::npos;
}

std::string process(std::string_view input, bool strip) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        char ch = input.at(i);

        // Escaped bracket: \[ => literal [
        if (ch == '\\' && i + 1 < input.size() && input.at(i + 1) == '[') {
            result += '[';
            ++i;
            continue;
        }

        if (ch == '[') {
            auto next = parseTag(input, i, result, strip);
            if (next != std::string_view::npos) {
                i = next - 1; // -1 because the loop will ++i
                continue;
            }
        }

        result += ch;
    }

    return result;
}

} // namespace

std::string markup(std::string_view input) { return process(input, false); }

std::string strip_markup(std::string_view input) { return process(input, true); }

} // namespace nodehammer

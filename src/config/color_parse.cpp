#include <config/color_parse.hpp>

#include <cmath>
#include <cstddef>

namespace nodehammer::config {

namespace {

// Hex colors are authored in sRGB; the renderer works in linear space.
float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Decode two hex digits at `offset` into a normalized [0,1] channel, optionally
// applying the sRGB→linear curve (alpha stays linear).
float hexByte(std::string_view s, std::size_t offset, bool linearize) {
    unsigned val = 0;
    for (int i = 0; i < 2; ++i) {
        const char c = s[offset + static_cast<std::size_t>(i)];
        val <<= 4;
        if (c >= '0' && c <= '9') {
            val += static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val += static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val += static_cast<unsigned>(c - 'A' + 10);
        }
    }
    const float f = static_cast<float>(val) / 255.0f;
    return linearize ? srgbToLinear(f) : f;
}

} // namespace

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::optional<Color> parseHexColor(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#') {
        hex.remove_prefix(1);
    }
    // Only exact #RRGGBB / #RRGGBBAA forms are valid, and every character must be
    // a hex digit — otherwise return nullopt so the caller reports the error
    // rather than silently decoding garbage (e.g. "#GGGGGG" or a 7-digit string).
    if (hex.size() != 6 && hex.size() != 8) {
        return std::nullopt;
    }
    for (const char c : hex) {
        if (!isHexDigit(c)) {
            return std::nullopt;
        }
    }
    Color c;
    c.r = hexByte(hex, 0, true);
    c.g = hexByte(hex, 2, true);
    c.b = hexByte(hex, 4, true);
    if (hex.size() == 8) {
        c.a = hexByte(hex, 6, false); // alpha is linear
    }
    return c;
}

} // namespace nodehammer::config

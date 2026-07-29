#pragma once

#include <config/config_ast.hpp>

#include <optional>
#include <string_view>

namespace nodehammer {

// ── BooleanFallback ─────────────────────────────────────────────────────────

inline std::string_view booleanFallbackToString(BooleanFallback f) {
    using enum BooleanFallback;
    switch (f) {
    case Skip:
        return "skip";
    case BBox:
        return "bbox";
    case Fail:
        return "fail";
    }
    return "skip";
}

inline std::optional<BooleanFallback> parseBooleanFallback(std::string_view s) {
    using enum BooleanFallback;
    if (s == "skip")
        return Skip;
    if (s == "bbox")
        return BBox;
    if (s == "fail")
        return Fail;
    return std::nullopt;
}

// ── AlphaMode ───────────────────────────────────────────────────────────────

inline std::string_view alphaModeToString(AlphaMode m) {
    using enum AlphaMode;
    switch (m) {
    case Opaque:
        return "opaque";
    case Mask:
        return "mask";
    case Blend:
        return "blend";
    }
    return "opaque";
}

inline std::optional<AlphaMode> parseAlphaMode(std::string_view s) {
    using enum AlphaMode;
    if (s == "opaque")
        return Opaque;
    if (s == "mask")
        return Mask;
    if (s == "blend")
        return Blend;
    return std::nullopt;
}

} // namespace nodehammer

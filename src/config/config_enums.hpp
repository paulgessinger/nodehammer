#pragma once

#include <config/config_ast.hpp>

#include <optional>
#include <string_view>

namespace nodehammer::config {

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

inline std::string_view shadingToString(Shading s) {
    switch (s) {
    case Shading::Preserve:
        return "preserve";
    case Shading::Smooth:
        return "smooth";
    case Shading::Flat:
        return "flat";
    }
    return "preserve";
}
inline std::optional<Shading> parseShading(std::string_view s) {
    if (s == "preserve")
        return Shading::Preserve;
    if (s == "smooth")
        return Shading::Smooth;
    if (s == "flat")
        return Shading::Flat;
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

} // namespace nodehammer::config

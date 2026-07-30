#pragma once

#include <config/config_ast.hpp>

#include <optional>
#include <string_view>

namespace nodehammer::config {

/// Parse a hex color string of the form "#RRGGBB" or "#RRGGBBAA" (the leading
/// '#' is optional). The RGB channels are converted from sRGB to linear; the
/// alpha channel is treated as already linear and defaults to 1.0 when absent.
/// Returns nullopt when the string has fewer than 6 hex digits, leaving the
/// caller to report the error in its own vocabulary.
///
/// Shared by the TOML loader and the Lua front-end so a `base_color = "#RRGGBB"`
/// resolves to identical linear values regardless of which front-end read it.
[[nodiscard]] std::optional<Color> parseHexColor(std::string_view hex);

} // namespace nodehammer::config

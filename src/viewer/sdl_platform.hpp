#pragma once

#include <bgfx/platform.h>

struct SDL_Window;

namespace nodehammer::viewer {

/// Populate bgfx::PlatformData from an SDL window. Per-OS the native handle
/// comes from a different SDL property; on emscripten the canvas is addressed
/// by selector string. Returns true on success.
bool fill_platform_data(SDL_Window *window, bgfx::PlatformData &out);

} // namespace nodehammer::viewer

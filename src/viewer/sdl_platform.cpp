#include "sdl_platform.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

namespace nodehammer::viewer {

bool fill_platform_data(SDL_Window *window, bgfx::PlatformData &out) {
    out = {};
#if defined(__EMSCRIPTEN__)
    (void)window;
    // bgfx pulls the WebGL2 context from the canvas selected here.
    out.nwh = const_cast<void *>(static_cast<const void *>("#canvas"));
    return true;
#else
    if (window == nullptr) {
        return false;
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (props == 0) {
        return false;
    }
#if defined(SDL_PLATFORM_MACOS)
    out.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    return out.nwh != nullptr;
#elif defined(SDL_PLATFORM_WIN32)
    out.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    return out.nwh != nullptr;
#elif defined(SDL_PLATFORM_LINUX)
    if (SDL_GetCurrentVideoDriver() != nullptr &&
        SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        out.ndt = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        out.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        out.type = bgfx::NativeWindowHandleType::Wayland;
    } else {
        out.ndt = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        out.nwh = reinterpret_cast<void *>(static_cast<uintptr_t>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
    }
    return out.nwh != nullptr;
#else
    return false;
#endif
#endif
}

} // namespace nodehammer::viewer

#pragma once

#include <nodehammer/viewer/platform.hpp>

namespace nodehammer::viewer::platform {

struct MacWindowCallbacks {
    void *user{nullptr};
    void (*set_chrome)(void *user, const WindowChromeState &chrome){nullptr};
    void (*set_drag_hover)(void *user, const DragHoverState &drag_hover){nullptr};
    void (*push_gesture)(void *user, const PlatformGestureEvent &event){nullptr};
};

void attachMacWindow(const WindowCustomizationRequest &request, MacWindowCallbacks callbacks);
void syncMacWindowState(MacWindowCallbacks callbacks);

} // namespace nodehammer::viewer::platform

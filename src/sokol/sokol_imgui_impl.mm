// Apple Objective-C++ flavour of sokol_imgui_impl.cc. Same source, just
// compiled with Objective-C++ because sokol_app.h's transitive AppKit
// imports require it.
#include "sokol_app.h"
#include "sokol_gfx.h"

#include "imgui.h"

#define SOKOL_IMPL
#include "util/sokol_imgui.h"

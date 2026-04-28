// Combined implementation TU for the C-only sokol single-headers.
// SOKOL_IMPL must appear in exactly ONE TU per program; each header has an
// internal "already implemented" guard so the order/grouping below emits
// each impl exactly once.
//
// Apple builds this same source list as sokol_impl.mm because sokol_app
// and sokol_gfx (Metal backend) call into Cocoa/Metal Objective-C APIs.
//
// sokol_imgui lives in its own TU (sokol_imgui_impl.cc / .mm) since it
// pulls in Dear ImGui, which is C++; that file includes sokol_app.h and
// sokol_gfx.h WITHOUT redefining SOKOL_IMPL so it picks up declarations
// only and doesn't redefine the symbols emitted here.
#define SOKOL_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"

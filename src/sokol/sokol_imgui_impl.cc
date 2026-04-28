// sokol_imgui implementation TU. Separated from sokol_impl.c because
// it needs C++ for imgui.h.
//
// Important ordering: sokol_app.h and sokol_gfx.h are included WITHOUT
// SOKOL_IMPL set, so they emit declarations only (their impls live in
// sokol_impl.c / sokol_impl.mm). Then SOKOL_IMPL is enabled and
// util/sokol_imgui.h is included — emitting the imgui backend impl exactly
// once.
#include "sokol_app.h"
#include "sokol_gfx.h"

#include "imgui.h"

#define SOKOL_IMPL
#include "util/sokol_imgui.h"

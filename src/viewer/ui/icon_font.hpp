#pragma once

// Re-exports the FA7 ICON_FA_* macros so any UI translation unit can include
// just this header to use icon glyphs — no need to know about the vendored
// third_party/fontawesome7/ path.
#include <IconsFontAwesome7.h>

namespace nodehammer::viewer::ui::icon_font {

/// Merges the vendored Font Awesome 7 solid font into the global ImGui font
/// atlas. Must be called once after `ImGui_ImplSokol_Init` and before the
/// first frame. Asserts on second call.
void initialize();

} // namespace nodehammer::viewer::ui::icon_font

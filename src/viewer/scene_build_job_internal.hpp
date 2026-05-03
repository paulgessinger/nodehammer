#pragma once

#include <string_view>

namespace nodehammer::viewer {

// Pre-build diagnostic line shared between the native and web SceneBuildJob
// implementations. Just logs the labels — the byte-driven build doesn't
// have real filesystem paths to inspect.
void logPreBuild(std::string_view config_label, std::string_view geometry_label);

} // namespace nodehammer::viewer

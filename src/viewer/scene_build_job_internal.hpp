#pragma once

#include <string>

namespace nodehammer::viewer {

// Pre-build diagnostic dump shared between the native and web SceneBuildJob
// implementations. Logs whether the config + input paths exist (and their
// sizes), and on failure walks the parent dirs to surface what actually
// landed there. Used to debug MEMFS sync issues on web and missing-file
// reports on native.
void logPreBuild(const std::string &config_path, const std::string &input_path);

} // namespace nodehammer::viewer

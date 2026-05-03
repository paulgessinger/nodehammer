#include "scene_build_job_internal.hpp"

#include <iostream>
#include <print>

namespace nodehammer::viewer {

void logPreBuild(std::string_view config_label, std::string_view geometry_label) {
    std::println(std::cerr, "scene_build_job: about to build — config={} geometry={}", config_label,
                 geometry_label);
}

} // namespace nodehammer::viewer

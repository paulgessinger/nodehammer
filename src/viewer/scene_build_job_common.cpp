#include "scene_build_job_internal.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <print>
#include <string>
#include <system_error>

namespace nodehammer::viewer {

void logPreBuild(const std::string &config_path, const std::string &input_path) {
    std::error_code ec;
    const bool config_exists = std::filesystem::exists(config_path, ec);
    const auto config_size = config_exists ? std::filesystem::file_size(config_path, ec) : 0;
    const bool input_exists = std::filesystem::exists(input_path, ec);
    const auto input_size = input_exists ? std::filesystem::file_size(input_path, ec) : 0;
    std::println(std::cerr,
                 "scene_build_job: about to build — config={} (exists={} size={}) input={} "
                 "(exists={} size={})",
                 config_path, config_exists ? 1 : 0, static_cast<size_t>(config_size), input_path,
                 input_exists ? 1 : 0, static_cast<size_t>(input_size));
    if (!config_exists || !input_exists) {
        for (const auto &p : {config_path, input_path}) {
            const auto parent = std::filesystem::path(p).parent_path();
            std::println(std::cerr, "scene_build_job:   listing {}:", parent.string());
            for (auto it = std::filesystem::directory_iterator(parent, ec);
                 !ec && it != std::filesystem::directory_iterator(); ++it) {
                std::println(std::cerr, "scene_build_job:     {}", it->path().string());
            }
            if (ec) {
                std::println(std::cerr, "scene_build_job:     (iteration error: {})", ec.message());
                ec.clear();
            }
        }
    }
}

} // namespace nodehammer::viewer

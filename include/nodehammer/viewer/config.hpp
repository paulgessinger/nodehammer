#pragma once

#include <cstdint>
#include <string>

namespace nodehammer::viewer {

struct Config {
    std::string title{"nodehammer viewer"};
    uint32_t width{1280};
    uint32_t height{720};
    bool vsync{true};
};

} // namespace nodehammer::viewer

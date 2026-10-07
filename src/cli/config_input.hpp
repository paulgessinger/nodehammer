#pragma once

#include <config/config_loader.hpp>
#include <diagnostic_codes.hpp>
#include <diagnostics.hpp>
#include <lua/lua_config.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

namespace nodehammer::cli::detail {

/// Read a whole file, or say which one could not be opened.
inline std::string readConfigScript(const std::string &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw nodehammer::Error{nodehammer::codes::kFatalCliFileOpen,
                                std::format("could not open '{}' for reading", path), path};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// Select the config language from its case-insensitive extension.
inline bool isLuaConfig(const std::string &path) {
    auto ext = std::filesystem::path{path}.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".lua";
}

/// Resolve TOML includes or evaluate Lua into the same config representation.
/// Lua evaluation collects errors; callers must check the returned diagnostics.
inline config::ConfigResult loadConfigFromFile(const std::string &path) {
    if (!isLuaConfig(path)) {
        return nodehammer::config::ConfigLoader::loadFromFile(path);
    }
    // The path the user typed is the root key, so include()/use() resolve
    // against the script's own directory — and a bare name roots at the working
    // directory, which is the application-level choice this command is entitled
    // to make on the user's behalf because they named the path relative to it.
    const std::string rootKey = std::filesystem::path{path}.lexically_normal().generic_string();
    return nodehammer::lua::evalLuaConfig(readConfigScript(path), rootKey,
                                          nodehammer::config::ConfigLoader::filesystemFetcher());
}

} // namespace nodehammer::cli::detail

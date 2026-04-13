#pragma once

#include <nodehammer/config/config_ast.hpp>

#include <string>

namespace nodehammer {

/// Serialize a NHConfig to a TOML string.
/// The output is a complete, valid TOML document that can be round-tripped
/// through ConfigLoader::loadFromString().
[[nodiscard]] std::string configToToml(const NHConfig &cfg);

} // namespace nodehammer

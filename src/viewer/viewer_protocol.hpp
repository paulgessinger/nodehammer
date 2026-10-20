#pragma once

#include <viewer/viewer_messages.hpp>

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace nodehammer::viewer::protocol {

/// Wire names are always `nh:` followed by the corresponding C++ message name.
template <typename Message> std::string typeName() {
    return std::string{"nh:"} + std::string{Message::name};
}

/// Decode a complete command envelope. Malformed/unknown messages return nullopt.
/// requestId is an optional nonnegative JavaScript-safe integer, defaulting to zero.
[[nodiscard]] std::optional<commands::Command> commandFromJson(const nlohmann::json &json);
/// Encode a typed event, preserving its name and response correlation ID.
[[nodiscard]] nlohmann::json eventToJson(const events::Event &event);

} // namespace nodehammer::viewer::protocol

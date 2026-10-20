#include <viewer/camera_json.hpp>
#include <viewer/viewer_protocol.hpp>

#include <nlohmann/json.hpp>

#include <type_traits>

namespace nodehammer::viewer::protocol {

std::optional<commands::Command> commandFromJson(const nlohmann::json &json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    const auto type = json.find("type");
    if (type == json.end() || !type->is_string()) {
        return std::nullopt;
    }
    if (*type == typeName<commands::SetCamera>()) {
        const auto pose = json.find("camera");
        if (pose == json.end()) {
            return std::nullopt;
        }
        const auto camera = cameraFromJson(*pose);
        if (!camera) {
            return std::nullopt;
        }
        const auto animate = json.find("animate");
        if (animate != json.end() && !animate->is_boolean()) {
            return std::nullopt;
        }
        return commands::SetCamera{*camera, animate != json.end() && animate->get<bool>()};
    }
    if (*type == typeName<commands::GetCamera>()) {
        const auto id = json.find("requestId");
        std::uint64_t request_id = 0;
        if (id != json.end()) {
            if (!id->is_number_integer() || *id < 0 || *id > 9007199254740991ULL) {
                return std::nullopt;
            }
            request_id = id->get<std::uint64_t>();
        }
        return commands::GetCamera{request_id};
    }
    return std::nullopt;
}

nlohmann::json eventToJson(const events::Event &event) {
    return std::visit(
        [](const auto &message) {
            using Message = std::decay_t<decltype(message)>;
            nlohmann::json result{{"type", typeName<Message>()},
                                  {"camera", cameraToJson(message.camera)}};
            if constexpr (std::is_same_v<Message, events::CameraResponse>) {
                result["requestId"] = message.request_id;
            }
            return result;
        },
        event);
}

} // namespace nodehammer::viewer::protocol

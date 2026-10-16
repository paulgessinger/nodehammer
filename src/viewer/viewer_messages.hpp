#pragma once

#include <viewer/camera.hpp>

#include <cstdint>
#include <string_view>
#include <variant>

namespace nodehammer::viewer {

namespace commands {
struct SetCamera {
    static constexpr std::string_view name = "SetCamera";
    Camera pose;
    bool animate{false};
};
struct GetCamera {
    static constexpr std::string_view name = "GetCamera";
    std::uint64_t request_id{0};
};
using Command = std::variant<SetCamera, GetCamera>;

} // namespace commands

namespace events {
struct ScenePresented {
    static constexpr std::string_view name = "ScenePresented";
    Camera camera;
};
struct CameraChanged {
    static constexpr std::string_view name = "CameraChanged";
    Camera camera;
};
struct CameraResponse {
    static constexpr std::string_view name = "CameraResponse";
    std::uint64_t request_id;
    Camera camera;
};
using Event = std::variant<ScenePresented, CameraChanged, CameraResponse>;

} // namespace events

} // namespace nodehammer::viewer

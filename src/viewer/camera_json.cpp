#include <viewer/camera_json.hpp>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace nodehammer::viewer {

namespace {

bool readFloat(const nlohmann::json &j, const char *key, float &out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return false;
    }
    out = it->get<float>();
    return true;
}

} // namespace

std::optional<Camera> cameraFromJson(const nlohmann::json &j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    Camera cam;
    float yaw_deg = 0.f;
    float pitch_deg = 0.f;
    if (!readFloat(j, "targetX", cam.target.x) || !readFloat(j, "targetY", cam.target.y) ||
        !readFloat(j, "targetZ", cam.target.z) || !readFloat(j, "distance", cam.distance) ||
        !readFloat(j, "yawDeg", yaw_deg) || !readFloat(j, "pitchDeg", pitch_deg)) {
        return std::nullopt;
    }
    if (cam.distance <= 0.f) {
        return std::nullopt;
    }
    cam.yaw = glm::radians(yaw_deg);
    cam.pitch = glm::radians(pitch_deg);
    if (const auto proj = j.find("projection"); proj != j.end() && proj->is_string()) {
        const auto value = proj->get<std::string>();
        if (value == "orthographic" || value == "ortho") {
            cam.projection = ProjectionMode::Orthographic;
        } else if (value == "perspective") {
            cam.projection = ProjectionMode::Perspective;
        }
    }
    return cam;
}

nlohmann::json cameraToJson(const Camera &camera) {
    return nlohmann::json{
        {"targetX", camera.target.x},
        {"targetY", camera.target.y},
        {"targetZ", camera.target.z},
        {"distance", camera.distance},
        {"yawDeg", glm::degrees(camera.yaw)},
        {"pitchDeg", glm::degrees(camera.pitch)},
        {"projection",
         camera.projection == ProjectionMode::Orthographic ? "orthographic" : "perspective"},
    };
}

bool sameCameraPose(const Camera &a, const Camera &b) {
    return a.target == b.target && a.distance == b.distance && a.yaw == b.yaw &&
           a.pitch == b.pitch && a.projection == b.projection;
}

} // namespace nodehammer::viewer

#include <viewer/camera_json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

using namespace nodehammer::viewer;
using Catch::Approx;

TEST_CASE("camera pose round-trips through JSON") {
    Camera camera;
    camera.target = {1.f, -2.f, 3.5f};
    camera.distance = 420.f;
    camera.yaw = glm::radians(35.f);
    camera.pitch = glm::radians(-20.f);
    camera.projection = ProjectionMode::Orthographic;

    const nlohmann::json j = cameraToJson(camera);
    CHECK(j.at("targetX").get<float>() == Approx(1.f));
    CHECK(j.at("targetY").get<float>() == Approx(-2.f));
    CHECK(j.at("targetZ").get<float>() == Approx(3.5f));
    CHECK(j.at("distance").get<float>() == Approx(420.f));
    CHECK(j.at("yawDeg").get<float>() == Approx(35.f));
    CHECK(j.at("pitchDeg").get<float>() == Approx(-20.f));
    CHECK(j.at("projection").get<std::string>() == "orthographic");
    // Exactly the wire vocabulary, nothing derived (fov / clip planes).
    CHECK(j.size() == 7);

    const auto restored = cameraFromJson(j);
    REQUIRE(restored.has_value());
    CHECK(restored->target.x == Approx(1.f));
    CHECK(restored->target.y == Approx(-2.f));
    CHECK(restored->target.z == Approx(3.5f));
    CHECK(restored->distance == Approx(420.f));
    CHECK(restored->yaw == Approx(camera.yaw));
    CHECK(restored->pitch == Approx(camera.pitch));
    CHECK(restored->projection == ProjectionMode::Orthographic);
}

TEST_CASE("camera JSON parse requires every pose field") {
    const nlohmann::json full = {
        {"targetX", 0.0},   {"targetY", 0.0}, {"targetZ", 0.0},
        {"distance", 10.0}, {"yawDeg", 0.0},  {"pitchDeg", 0.0},
    };
    REQUIRE(cameraFromJson(full).has_value());

    for (const char *key : {"targetX", "targetY", "targetZ", "distance", "yawDeg", "pitchDeg"}) {
        nlohmann::json partial = full;
        partial.erase(key);
        CHECK_FALSE(cameraFromJson(partial).has_value());
    }

    nlohmann::json non_numeric = full;
    non_numeric["yawDeg"] = "45";
    CHECK_FALSE(cameraFromJson(non_numeric).has_value());

    CHECK_FALSE(cameraFromJson(nlohmann::json::array()).has_value());
    CHECK_FALSE(cameraFromJson(nlohmann::json(nullptr)).has_value());
}

TEST_CASE("camera JSON parse rejects a non-positive distance") {
    nlohmann::json j = {
        {"targetX", 0.0},  {"targetY", 0.0}, {"targetZ", 0.0},
        {"distance", 0.0}, {"yawDeg", 0.0},  {"pitchDeg", 0.0},
    };
    CHECK_FALSE(cameraFromJson(j).has_value());
    j["distance"] = -1.0;
    CHECK_FALSE(cameraFromJson(j).has_value());
}

TEST_CASE("camera JSON projection defaults to perspective and accepts synonyms") {
    nlohmann::json j = {
        {"targetX", 0.0},   {"targetY", 0.0}, {"targetZ", 0.0},
        {"distance", 10.0}, {"yawDeg", 90.0}, {"pitchDeg", 10.0},
    };
    auto cam = cameraFromJson(j);
    REQUIRE(cam.has_value());
    CHECK(cam->projection == ProjectionMode::Perspective);
    CHECK(cam->yaw == Approx(glm::half_pi<float>()));

    j["projection"] = "ortho";
    cam = cameraFromJson(j);
    REQUIRE(cam.has_value());
    CHECK(cam->projection == ProjectionMode::Orthographic);

    j["projection"] = "orthographic";
    cam = cameraFromJson(j);
    REQUIRE(cam.has_value());
    CHECK(cam->projection == ProjectionMode::Orthographic);

    // Unknown strings leave the default alone rather than failing the parse.
    j["projection"] = "fisheye";
    cam = cameraFromJson(j);
    REQUIRE(cam.has_value());
    CHECK(cam->projection == ProjectionMode::Perspective);
}

TEST_CASE("sameCameraPose compares only the exchanged pose fields") {
    Camera a;
    Camera b;
    CHECK(sameCameraPose(a, b));

    b.fov_deg = 30.f; // derived, not part of the pose
    b.near_plane = 1.f;
    CHECK(sameCameraPose(a, b));

    b.yaw = 0.1f;
    CHECK_FALSE(sameCameraPose(a, b));
    b = a;
    b.projection = ProjectionMode::Orthographic;
    CHECK_FALSE(sameCameraPose(a, b));
    b = a;
    b.target.z = 1.f;
    CHECK_FALSE(sameCameraPose(a, b));
}

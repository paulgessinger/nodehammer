#include <viewer/camera_json.hpp>
#include <viewer/viewer_protocol.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace nodehammer::viewer;

TEST_CASE("viewer protocol command names match their C++ types") {
    Camera pose;
    pose.target.x = 42.f;
    nlohmann::json message{{"type", "nh:SetCamera"}, {"camera", cameraToJson(pose)}};
    auto command = protocol::commandFromJson(message);
    REQUIRE(command);
    CHECK(std::get<commands::SetCamera>(*command).pose.target.x == 42.f);
    CHECK_FALSE(std::get<commands::SetCamera>(*command).animate);
    message["animate"] = true;
    command = protocol::commandFromJson(message);
    REQUIRE(command);
    CHECK(std::get<commands::SetCamera>(*command).animate);
    message["animate"] = "true";
    CHECK_FALSE(protocol::commandFromJson(message));
    message.erase("animate");
    message["camera"].erase("distance");
    CHECK_FALSE(protocol::commandFromJson(message));
    CHECK_FALSE(protocol::commandFromJson({{"type", "nh:setCamera"}}));
    CHECK_FALSE(protocol::commandFromJson(nullptr));
}

TEST_CASE("viewer protocol camera requests preserve safe integer correlation IDs") {
    auto command = protocol::commandFromJson({{"type", "nh:GetCamera"}});
    REQUIRE(command);
    CHECK(std::get<commands::GetCamera>(*command).request_id == 0);
    for (const auto id : {42ULL, 9007199254740991ULL}) {
        command = protocol::commandFromJson({{"type", "nh:GetCamera"}, {"requestId", id}});
        REQUIRE(command);
        CHECK(std::get<commands::GetCamera>(*command).request_id == id);
        const auto response = protocol::eventToJson(events::CameraResponse{id, Camera{}});
        CHECK(response.at("type") == "nh:CameraResponse");
        CHECK(response.at("requestId") == id);
    }
    for (const auto &id : nlohmann::json::array({-1, 1.5, "42", nullptr, 9007199254740992ULL})) {
        CHECK_FALSE(protocol::commandFromJson({{"type", "nh:GetCamera"}, {"requestId", id}}));
    }
}

TEST_CASE("viewer protocol event envelopes preserve event names and camera fields") {
    Camera pose;
    pose.target.y = 17.f;
    const auto ready = protocol::eventToJson(events::ScenePresented{pose});
    CHECK(ready.at("type") == "nh:ScenePresented");
    CHECK(ready.at("camera").at("targetY") == 17.f);
    CHECK_FALSE(ready.contains("requestId"));
    const auto changed = protocol::eventToJson(events::CameraChanged{pose});
    CHECK(changed.at("type") == "nh:CameraChanged");
    CHECK(changed.at("camera") == ready.at("camera"));
    CHECK_FALSE(changed.contains("requestId"));
}

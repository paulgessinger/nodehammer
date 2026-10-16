#include <viewer/viewer_connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif

using namespace nodehammer::viewer;

#ifndef __EMSCRIPTEN__
TEST_CASE("viewer commands can be submitted from a controller thread in order") {
    ViewerConnection connection;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < 100; ++i) {
            Camera pose;
            pose.target.x = static_cast<float>(i);
            connection.submit(commands::SetCamera{pose, true});
            connection.submit(commands::GetCamera{i});
        }
        done = true;
    });
    std::vector<commands::Command> received;
    while (!done) {
        auto batch = connection.takeCommands();
        received.insert(received.end(), batch.begin(), batch.end());
        std::this_thread::yield();
    }
    producer.join();
    auto remaining = connection.takeCommands();
    received.insert(received.end(), remaining.begin(), remaining.end());
    REQUIRE(received.size() == 200);
    for (std::size_t i = 0; i < 100; ++i) {
        CHECK(std::get<commands::SetCamera>(received.at(2 * i)).pose.target.x ==
              static_cast<float>(i));
        CHECK(std::get<commands::GetCamera>(received.at(2 * i + 1)).request_id == i);
    }
    CHECK(connection.takeCommands().empty());
}

#endif

TEST_CASE("viewer events coalesce motion without dropping responses or scene readiness") {
    ViewerConnection connection;
    Camera a;
    Camera b;
    b.target.x = 5.f;
    connection.publish(events::ScenePresented{a});
    connection.publish(events::CameraChanged{a});
    connection.publish(events::CameraChanged{b});
    connection.publish(events::CameraResponse{42, b});
    connection.publish(events::CameraChanged{a});
    const auto events = connection.takeEvents();
    REQUIRE(events.size() == 4);
    CHECK(std::holds_alternative<events::ScenePresented>(events.at(0)));
    CHECK(std::get<events::CameraChanged>(events.at(1)).camera.target.x == 5.f);
    CHECK(std::get<events::CameraResponse>(events.at(2)).request_id == 42);
    CHECK(std::get<events::CameraChanged>(events.at(3)).camera.target.x == 0.f);
    CHECK(connection.takeEvents().empty());
}

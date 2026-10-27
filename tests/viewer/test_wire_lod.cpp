#include <viewer/wire_lod.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace nodehammer;
using namespace nodehammer::viewer;

namespace {
detail::JsonValue metadata(float radius = 0.01f, float length = 1, float pixels = 0.5f) {
    return detail::JsonValue::makeObject(
        {{"nodehammer_wire_lod",
          detail::JsonValue::makeObject(
              {{"radius", radius}, {"half_length", length}, {"min_pixels", pixels}})}});
}
const auto perspective = glm::perspective(glm::radians(90.f), 1.f, 0.1f, 1000.f);
float fade(const WireLodBounds &wire) {
    return wireLodFade(wire, glm::mat4{1}, perspective, 1000, 1000, true, 0.1f);
}
} // namespace

TEST_CASE("wire LOD measures radial thickness with a smooth visibility band",
          "[viewer][wire_lod]") {
    auto bounds = wireLodBounds(metadata(), glm::translate(glm::mat4{1}, {0, 0, -100}));
    CHECK(fade(bounds) == 1); // diameter ~0.1px
    bounds = wireLodBounds(metadata(), glm::translate(glm::mat4{1}, {0, 0, -5}));
    CHECK(fade(bounds) == 0);
    bounds = wireLodBounds(metadata(), glm::translate(glm::mat4{1}, {0, 0, -15}));
    CHECK(fade(bounds) > 0);
    CHECK(fade(bounds) < 1);
    CHECK(wireLodFade(bounds, glm::mat4{1}, perspective, 2000, 2000, true, 0.1f) == 0);
}

TEST_CASE("wire LOD does not confuse wire length with thickness", "[viewer][wire_lod]") {
    auto world = glm::translate(glm::mat4{1}, {0, 0, -100});
    world = glm::rotate(world, glm::radians(90.f), glm::vec3{0, 1, 0});
    const auto shortWire = wireLodBounds(metadata(), world);
    const auto longWire = wireLodBounds(metadata(), glm::scale(world, {1, 1, 100}));
    CHECK(shortWire.radius == Catch::Approx(longWire.radius));
    CHECK(fade(shortWire) == Catch::Approx(fade(longWire)));
    CHECK(fade(longWire) == 1);
}

TEST_CASE("wire LOD keeps nearby portions and supports orthographic zoom", "[viewer][wire_lod]") {
    const auto wire =
        wireLodBounds(metadata(0.01f, 100), glm::translate(glm::mat4{1}, {0, 0, -100}));
    CHECK(fade(wire) == 0); // A wire endpoint touches the near plane.
    const auto orthoFar = glm::ortho(-100.f, 100.f, -100.f, 100.f, 0.1f, 1000.f);
    const auto orthoNear = glm::ortho(-1.f, 1.f, -1.f, 1.f, 0.1f, 1000.f);
    CHECK(wireLodFade(wire, glm::mat4{1}, orthoFar, 1000, 1000, false, 0.1f) == 1);
    CHECK(wireLodFade(wire, glm::mat4{1}, orthoNear, 1000, 1000, false, 0.1f) == 0);
}

TEST_CASE("wire LOD handles affine radial scaling and malformed metadata conservatively",
          "[viewer][wire_lod]") {
    auto world = glm::scale(glm::mat4{1}, {2, 3, 1000});
    CHECK(wireLodBounds(metadata(1), world).radius == Catch::Approx(3));
    world[1][0] = 2;
    CHECK(wireLodBounds(metadata(1), world).radius > 3);
    CHECK(fade(wireLodBounds(detail::JsonValue{}, world)) == 0);
    CHECK(fade(wireLodBounds(metadata(1, 1, 0), world)) == 0);
    CHECK(fade(wireLodBounds(metadata(-1), world)) == 0);
    CHECK(fade(wireLodBounds(metadata(1, -1), world)) == 0);
}

TEST_CASE("wire spatial blocks conservatively enclose wires and keep stable nested samples",
          "[viewer][wire_lod]") {
    std::vector<WireLodBounds> wires;
    for (int i = 0; i < 1024; ++i) {
        float x = static_cast<float>(i % 32), y = static_cast<float>(i / 32);
        wires.push_back({{x, y, -100}, {x, y, -20}, 0.01f, 1.5f, 100});
    }
    const auto index = buildWireLodIndex(wires);
    REQUIRE(index.blocks.size() == 16);
    CHECK(buildWireLodIndex(wires).order == index.order);
    std::vector<bool> seen(wires.size());
    size_t tenth = 0, third = 0;
    for (const auto &block : index.blocks) {
        float previous = -1;
        for (size_t n = block.begin; n < block.end; ++n) {
            const auto i = index.order[n];
            CHECK_FALSE(seen[i]);
            seen[i] = true;
            CHECK(index.ranks[i] >= previous);
            previous = index.ranks[i];
            tenth += index.ranks[i] < 0.1f;
            third += index.ranks[i] < 0.3f;
            for (int axis = 0; axis < 3; ++axis) {
                CHECK(block.min[axis] <=
                      std::min(wires[i].start[axis], wires[i].end[axis]) - wires[i].radius);
                CHECK(block.max[axis] >=
                      std::max(wires[i].start[axis], wires[i].end[axis]) + wires[i].radius);
            }
        }
        if (wireBlockBelowPixels(block, glm::mat4{1}, 1000, true, 0.1f))
            for (size_t n = block.begin; n < block.end; ++n)
                CHECK(fade(wires[index.order[n]]) == 1);
    }
    CHECK(tenth > 70);
    CHECK(tenth < 140);
    CHECK(third > 250);
    CHECK(third < 370);
    wires.front().maxDistance = 0;
    CHECK(buildWireLodIndex(wires).blocks.empty()); // Mixed/disabled policy stays on legacy path.
}

TEST_CASE("wire distance uses nearest segment and sparse distant hint with nested fractions",
          "[viewer][wire_lod]") {
    WireLodBounds wire{{0, 0, -100}, {0, 0, 100}, 0.01f, 0, 100};
    CHECK(wirePointDistance(wire, {10, 0, 0}) == Catch::Approx(9.99f));
    CHECK(wireDistanceFraction(0, 100) == 1);
    CHECK(wireDistanceFraction(25, 100) == 0.3f);
    CHECK(wireDistanceFraction(50, 100) == 0.1f);
    CHECK(wireDistanceFraction(100, 100) == kWireHintFraction);
    CHECK(wireDistanceFraction(10000, 100) == kWireHintFraction);
    auto data = metadata();
    auto lod = *data.find("nodehammer_wire_lod");
    lod.set("max_distance", 100.);
    data.set("nodehammer_wire_lod", lod);
    CHECK(wireLodBounds(data, glm::mat4{1}).maxDistance == 100);
}

TEST_CASE("wire hints relax thickness only for the stable sparse subset", "[viewer][wire_lod]") {
    int hints = 0;
    for (int i = 0; i < 10000; ++i) {
        WireLodBounds wire{{float(i), 0, -100}, {float(i), 0, 100}, 0.01f, 1.5f, 100};
        const auto hint = wireHintBounds(wire);
        if (wireSampleRank(wire) < kWireHintFraction) {
            CHECK(hint.minPixels == 0);
            ++hints;
            CHECK(wireHintScale(hint, glm::translate(glm::mat4{1}, {0, 0, -1000}), 1000, true,
                                0.1f) > 1);
        } else
            CHECK(hint.minPixels == wire.minPixels);
        wire.maxDistance = 0;
        CHECK(wireHintBounds(wire).minPixels == 1.5f);
    }
    CHECK(hints > 30);
    CHECK(hints < 75);
}

TEST_CASE("wire stepped density has a ten percent band beginning at twenty centimetres",
          "[viewer][wire_lod]") {
    CHECK(wireDistanceFraction(9.99f, 40) == 1);
    CHECK(wireDistanceFraction(10, 40) == 0.3f);
    CHECK(wireDistanceFraction(19.99f, 40) == 0.3f);
    CHECK(wireDistanceFraction(20, 40) == 0.1f);
    CHECK(wireDistanceFraction(39.99f, 40) == 0.1f);
    CHECK(wireDistanceFraction(40, 40) == 0.005f);
    CHECK(wireDistanceFade(0.004f, 1000, 40) == 0);
    CHECK(wireDistanceFade(0.006f, 1000, 40) == 1);
    CHECK(wireDistanceFade(0.11f, 20, 40) == 1);
    CHECK(wireDistanceFade(0.09f, 20, 40) == 0);
}

TEST_CASE("custom wire policies retain conservative mixed spatial blocks", "[viewer][wire_lod]") {
    std::vector<WireLodBounds> wires;
    for (int i = 0; i < 100; ++i) {
        WireLodBounds w{{float(i), 0, -10}, {float(i), 0, 10}, 0.01f, 1.5f, 40};
        w.policy.factors =
            i % 2 ? std::array<float, 3>{0.1f, 0.2f, 4} : std::array<float, 3>{0.3f, 2, 3};
        w.policy.fractions = i % 2 ? std::array<float, 4>{0.8f, 0.7f, 0.1f, 0}
                                   : std::array<float, 4>{1, 0.3f, 0.2f, 0.02f};
        wires.push_back(wireHintBounds(w));
    }
    auto index = buildWireLodIndex(wires);
    for (const auto &block : index.blocks)
        for (float d = 0; d < 200; d += 1)
            for (size_t n = block.begin; n < block.end; ++n) {
                const auto &w = wires[index.order[n]];
                CHECK(wireDistanceFraction(d, block.maxDistance, block.policy) >=
                      wireDistanceFraction(d, w.maxDistance, w.policy));
            }
    WireLodBounds w{{0, 0, -100}, {0, 0, -50}, 0.01f, 0, 40};
    w.hint = true;
    w.policy.hintPixels = 0;
    CHECK(wireHintScale(w, glm::mat4{1}, 1000, true, 0.1f) == 1);
    w.policy.hintPixels = 1;
    CHECK(wireHintScale(w, glm::mat4{1}, 1000, true, 0.1f) > 1);
}

TEST_CASE("wire policy metadata reaches renderer without changing defaults", "[viewer][wire_lod]") {
    auto extras = metadata();
    auto lod = *extras.find("nodehammer_wire_lod");
    lod.set("distance_factors", detail::JsonValue::makeArray({0.1, 0.4, 2.0}));
    lod.set("fractions", detail::JsonValue::makeArray({0.8, 0.2, 0.05, 0.0}));
    lod.set("hint_pixels", 0.75);
    extras.set("nodehammer_wire_lod", lod);
    auto wire = wireLodBounds(extras, glm::mat4{1});
    CHECK(wire.policy.factors == std::array<float, 3>{0.1f, 0.4f, 2});
    CHECK(wire.policy.fractions == std::array<float, 4>{0.8f, 0.2f, 0.05f, 0});
    CHECK(wire.policy.hintPixels == 0.75f);
    lod.set("fractions", detail::JsonValue::makeArray({1.0, 0.1, 0.2, 0.0}));
    extras.set("nodehammer_wire_lod", lod);
    CHECK(wireLodBounds(extras, glm::mat4{1}).policy.fractions == WireLodPolicy{}.fractions);
}

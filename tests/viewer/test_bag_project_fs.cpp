#include <catch2/catch_test_macros.hpp>

#include <viewer/bag_project_fs.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using nodehammer::viewer::BagProjectFs;
using Kind = nodehammer::viewer::ProjectDropDecision::Kind;
using nodehammer::viewer::ResolveStatus;

namespace {

std::vector<std::byte> bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string asString(std::span<const std::byte> data) {
    return {reinterpret_cast<const char *>(data.data()), data.size()};
}

} // namespace

TEST_CASE("BagProjectFs replaces duplicate filenames without duplicate entries",
          "[viewer][bag_project_fs]") {
    BagProjectFs bag;
    auto first = bytes("first");
    auto second = bytes("second");

    bag.addBytes("scene.toml", std::span<const std::byte>{first});
    const auto gen0 = bag.generation();

    REQUIRE(bag.planAddBytes("other.toml", std::span<const std::byte>{second}).kind ==
            Kind::Accept);
    REQUIRE(bag.planAddBytes("SCENE.toml", std::span<const std::byte>{second}).kind ==
            Kind::Confirm);
    bag.addBytes("SCENE.toml", std::span<const std::byte>{second});

    REQUIRE(bag.generation() > gen0);
    REQUIRE(bag.progress().size() == 1);
    REQUIRE(bag.list("").size() == 1);
    REQUIRE(bag.progress().front().url == "SCENE.toml");
    REQUIRE(bag.list("").front().key == "SCENE.toml");
    REQUIRE_FALSE(bag.warnings().empty());

    auto resolved = bag.resolve("scene.toml");
    REQUIRE(resolved.status == ResolveStatus::Ready);
    REQUIRE(asString(resolved.file.bytes.span()) == "second");
}

TEST_CASE("BagProjectFs ByteBuffer outlives mutation", "[viewer][bag_project_fs]") {
    BagProjectFs bag;
    auto first = bytes("first");
    bag.addBytes("scene.toml", std::span<const std::byte>{first});

    auto r1 = bag.resolve("scene.toml");
    REQUIRE(r1.status == ResolveStatus::Ready);
    auto buf1 = r1.file.bytes;

    auto second = bytes("second");
    bag.addBytes("scene.toml", std::span<const std::byte>{second});
    auto extra = bytes("x");
    bag.addBytes("other.toml", std::span<const std::byte>{extra});

    REQUIRE(asString(buf1.span()) == "first");

    auto r2 = bag.resolve("scene.toml");
    REQUIRE(asString(r2.file.bytes.span()) == "second");
}

TEST_CASE("BagProjectFs repeated resolve returns the same buffer", "[viewer][bag_project_fs]") {
    BagProjectFs bag;
    auto data = bytes("payload");
    bag.addBytes("scene.toml", std::span<const std::byte>{data});

    auto a = bag.resolve("scene.toml").file.bytes;
    auto b = bag.resolve("scene.toml").file.bytes;
    REQUIRE(a.span().data() == b.span().data());
}

#include <catch2/catch_test_macros.hpp>

#include <detail/file_io.hpp>
#include <viewer/native_bag_project_fs.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using nodehammer::viewer::NativeBagProjectFs;
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

TEST_CASE("NativeBagProjectFs writes drops through to disk", "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
    REQUIRE(std::filesystem::is_directory(bag.storageDir()));

    auto contents = bytes("scene-bytes");
    bag.addBytes("scene.toml", std::span<const std::byte>{contents});

    const auto on_disk = bag.storageDir() / "scene.toml";
    REQUIRE(std::filesystem::is_regular_file(on_disk));
    auto disk_bytes = nodehammer::file_io::readFile(on_disk);
    REQUIRE(asString(std::span<const std::byte>{disk_bytes}) == "scene-bytes");
}

TEST_CASE("NativeBagProjectFs resolves drops via inner FS read",
          "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
    auto contents = bytes("hello");
    bag.addBytes("scene.toml", std::span<const std::byte>{contents});

    auto r = bag.resolve("scene.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    REQUIRE(asString(r.file.bytes.span()) == "hello");
}

TEST_CASE("NativeBagProjectFs list reflects current on-disk state",
          "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
    auto a = bytes("a");
    auto b = bytes("b");
    bag.addBytes("a.toml", std::span<const std::byte>{a});
    bag.addBytes("b.toml", std::span<const std::byte>{b});

    auto top = bag.list("");
    REQUIRE(top.size() == 2);
    bool saw_a = false, saw_b = false;
    for (const auto &n : top) {
        if (n.name == "a.toml")
            saw_a = true;
        if (n.name == "b.toml")
            saw_b = true;
    }
    REQUIRE(saw_a);
    REQUIRE(saw_b);
}

TEST_CASE("NativeBagProjectFs replaces duplicate filenames without duplicate entries",
          "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
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
    REQUIRE(bag.progress().front().url == "SCENE.toml");
    REQUIRE_FALSE(bag.warnings().empty());

    auto resolved = bag.resolve("SCENE.toml");
    REQUIRE(resolved.status == ResolveStatus::Ready);
    REQUIRE(asString(resolved.file.bytes.span()) == "second");
}

TEST_CASE("NativeBagProjectFs subdir-key resolve falls back to flat basename drop",
          "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
    auto contents = bytes("included");
    bag.addBytes("common.toml", std::span<const std::byte>{contents});

    auto r = bag.resolve("subdir/common.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    REQUIRE(asString(r.file.bytes.span()) == "included");
    REQUIRE(r.file.key == "subdir/common.toml");
}

TEST_CASE("NativeBagProjectFs generation bumps on every drop", "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
    const auto g0 = bag.generation();

    auto a = bytes("a");
    bag.addBytes("a.toml", std::span<const std::byte>{a});
    const auto g1 = bag.generation();
    REQUIRE(g1 > g0);

    auto b = bytes("b");
    bag.addBytes("b.toml", std::span<const std::byte>{b});
    REQUIRE(bag.generation() > g1);
}

TEST_CASE("NativeBagProjectFs cleans its storage dir on destruction",
          "[viewer][native_bag_project_fs]") {
    std::filesystem::path captured;
    {
        NativeBagProjectFs bag;
        captured = bag.storageDir();
        auto data = bytes("x");
        bag.addBytes("x.toml", std::span<const std::byte>{data});
        REQUIRE(std::filesystem::is_directory(captured));
    }
    REQUIRE_FALSE(std::filesystem::exists(captured));
}

TEST_CASE("NativeBagProjectFs instances do not share storage", "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs a;
    NativeBagProjectFs b;
    REQUIRE(a.storageDir() != b.storageDir());

    auto data = bytes("only-in-a");
    a.addBytes("scene.toml", std::span<const std::byte>{data});

    REQUIRE(a.resolve("scene.toml").status == ResolveStatus::Ready);
    REQUIRE(b.resolve("scene.toml").status == ResolveStatus::Missing);
}

TEST_CASE("NativeBagProjectFs rejects path-shaped filenames", "[viewer][native_bag_project_fs]") {
    NativeBagProjectFs bag;
    auto data = bytes("payload");
    bag.addBytes("with/slash.toml", std::span<const std::byte>{data});
    bag.addBytes("with\\backslash.toml", std::span<const std::byte>{data});
    bag.addBytes("..", std::span<const std::byte>{data});

    REQUIRE(bag.progress().empty());
    REQUIRE(bag.list("").empty());
}

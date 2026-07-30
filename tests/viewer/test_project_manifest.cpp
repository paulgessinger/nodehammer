#include <viewer/project_manifest.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using nodehammer::viewer::parseProjectManifest;

namespace {

std::vector<std::byte> asBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

} // namespace

TEST_CASE("parseProjectManifest reads [project] entry keys", "[viewer][project_manifest]") {
    auto toml = asBytes("[project]\nconfig = \"scene.toml\"\ngeometry = \"scene.nhb.zst\"\n"
                        "title = \"ODD\"\n\n[view]\ncamera = { fov = 45 }\n");
    auto m = parseProjectManifest(toml);
    REQUIRE(m.has_value());
    REQUIRE(m->config_key == "scene.toml");
    REQUIRE(m->geometry_key == "scene.nhb.zst");
}

TEST_CASE("parseProjectManifest requires both entry keys", "[viewer][project_manifest]") {
    // Config only → nullopt (caller falls back to recognition).
    REQUIRE_FALSE(
        parseProjectManifest(asBytes("[project]\nconfig = \"scene.toml\"\n")).has_value());
    // Geometry only → nullopt.
    REQUIRE_FALSE(
        parseProjectManifest(asBytes("[project]\ngeometry = \"scene.nhb.zst\"\n")).has_value());
    // No [project] section → nullopt.
    REQUIRE_FALSE(parseProjectManifest(asBytes("[view]\ncamera = {}\n")).has_value());
}

TEST_CASE("parseProjectManifest returns nullopt on malformed TOML", "[viewer][project_manifest]") {
    REQUIRE_FALSE(parseProjectManifest(asBytes("this = = not toml [[[\n")).has_value());
    REQUIRE_FALSE(parseProjectManifest(asBytes("")).has_value());
}

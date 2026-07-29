#include <catch2/catch_test_macros.hpp>
#include <ir/render/exporter.hpp>

using namespace nodehammer;

TEST_CASE("RenderExporterRegistry resolves by extension", "[export][render]") {
    const auto reg = RenderExporterRegistry::makeDefault();

    REQUIRE(reg.resolve("out.glb") != nullptr);
    REQUIRE(reg.resolve("out.glb")->formatName() == "gltf");

    REQUIRE(reg.resolve("out.gltf") != nullptr);
    REQUIRE(reg.resolve("out.gltf")->formatName() == "gltf");

    REQUIRE(reg.resolve("out.obj") != nullptr);
    REQUIRE(reg.resolve("out.obj")->formatName() == "obj");
}

TEST_CASE("RenderExporterRegistry resolves by explicit format", "[export][render]") {
    const auto reg = RenderExporterRegistry::makeDefault();

    REQUIRE(reg.resolve("out.unknown", "gltf") != nullptr);
    REQUIRE(reg.resolve("out.unknown", "gltf")->formatName() == "gltf");

    REQUIRE(reg.resolve("out.unknown", "obj") != nullptr);
    REQUIRE(reg.resolve("out.unknown", "obj")->formatName() == "obj");

    REQUIRE(reg.resolve("out.unknown", "bogus") == nullptr);
}

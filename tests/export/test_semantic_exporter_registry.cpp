#include <catch2/catch_test_macros.hpp>
#include <ir/semantic/exporter.hpp>

using namespace nodehammer;
using namespace nodehammer::ir;

TEST_CASE("SemanticExporterRegistry resolves by extension", "[export][semantic]") {
    const auto reg = SemanticExporterRegistry::makeDefault();

    REQUIRE(reg.resolve("out.json") != nullptr);
    REQUIRE(reg.resolve("out.json")->formatName() == "json");

    REQUIRE(reg.resolve("out.json.zst") != nullptr);
    REQUIRE(reg.resolve("out.json.zst")->formatName() == "json");

    REQUIRE(reg.resolve("out.nhb") != nullptr);
    REQUIRE(reg.resolve("out.nhb")->formatName() == "nhb");

    REQUIRE(reg.resolve("out.nhb.zst") != nullptr);
    REQUIRE(reg.resolve("out.nhb.zst")->formatName() == "nhb");
}

TEST_CASE("SemanticExporterRegistry resolves by explicit format", "[export][semantic]") {
    const auto reg = SemanticExporterRegistry::makeDefault();

    REQUIRE(reg.resolve("whatever.unknown", "json") != nullptr);
    REQUIRE(reg.resolve("whatever.unknown", "json")->formatName() == "json");

    REQUIRE(reg.resolve("whatever.unknown", "nhb") != nullptr);
    REQUIRE(reg.resolve("whatever.unknown", "nhb")->formatName() == "nhb");

    REQUIRE(reg.resolve("whatever.unknown", "bogus") == nullptr);
}

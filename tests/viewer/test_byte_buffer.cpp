#include <catch2/catch_test_macros.hpp>

#include <nodehammer/viewer/byte_buffer.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using nodehammer::viewer::ByteBuffer;

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

TEST_CASE("ByteBuffer default-constructs empty", "[viewer][byte_buffer]") {
    ByteBuffer buf;
    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);
    REQUIRE(buf.span().empty());
}

TEST_CASE("ByteBuffer wraps a moved-in vector", "[viewer][byte_buffer]") {
    ByteBuffer buf{bytes("hello")};
    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 5);
    REQUIRE(asString(buf.span()) == "hello");
}

TEST_CASE("ByteBuffer copy shares storage", "[viewer][byte_buffer]") {
    ByteBuffer a{bytes("shared")};
    ByteBuffer b = a;
    REQUIRE(a.span().data() == b.span().data());
    REQUIRE(asString(b.span()) == "shared");
}

TEST_CASE("ByteBuffer move transfers ownership", "[viewer][byte_buffer]") {
    ByteBuffer a{bytes("moved")};
    const auto *original_ptr = a.span().data();
    ByteBuffer b = std::move(a);
    REQUIRE(b.span().data() == original_ptr);
    REQUIRE(asString(b.span()) == "moved");
}

TEST_CASE("ByteBuffer outlives the producer's handle", "[viewer][byte_buffer]") {
    ByteBuffer kept;
    {
        ByteBuffer producer{bytes("survives")};
        kept = producer;
    }
    REQUIRE(asString(kept.span()) == "survives");
}

#include <nodehammer/viewer/zip_working_set.hpp>

#include <catch2/catch_test_macros.hpp>

#include <miniz.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using nodehammer::viewer::ZipDirEntry;
using nodehammer::viewer::ZipWorkingSet;

namespace {

std::vector<std::byte> asBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string asString(std::span<const std::byte> sp) {
    return std::string{reinterpret_cast<const char *>(sp.data()), sp.size()};
}

/// Author a ZIP blob in memory from (name, contents) pairs using miniz directly,
/// so the ZipWorkingSet tests start from a real archive they didn't produce.
std::vector<std::byte> makeZip(const std::vector<std::pair<std::string, std::string>> &entries) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto &[name, contents] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    }
    void *ptr = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &ptr, &size));
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), ptr, size);
    mz_zip_writer_end(&zip);
    return out;
}

const ZipDirEntry *find(std::span<const ZipDirEntry> entries, std::string_view name) {
    auto it = std::ranges::find_if(entries, [&](const ZipDirEntry &e) { return e.name == name; });
    return it == entries.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("ZipWorkingSet reads original entries and passes them through serialize",
          "[viewer][zip_working_set]") {
    auto blob = makeZip({{"top.toml", "root = 1\n"}, {"a/b.toml", "nested = 2\n"}});
    auto ws = ZipWorkingSet::openFromBytes(blob);

    REQUIRE(ws.contains("top.toml"));
    REQUIRE(ws.contains("a/b.toml"));
    REQUIRE_FALSE(ws.contains("missing.toml"));
    REQUIRE_FALSE(ws.dirty());

    auto top = ws.read("top.toml");
    REQUIRE(top.has_value());
    REQUIRE(asString(top->span()) == "root = 1\n");
    REQUIRE(asString(ws.read("a/b.toml")->span()) == "nested = 2\n");
    REQUIRE_FALSE(ws.read("missing.toml").has_value());

    // A clean serialize round-trips every original entry unchanged.
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());
    REQUIRE(asString(out.read("top.toml")->span()) == "root = 1\n");
    REQUIRE(asString(out.read("a/b.toml")->span()) == "nested = 2\n");
}

TEST_CASE("ZipWorkingSet overlays writes and removals over the archive",
          "[viewer][zip_working_set]") {
    auto blob = makeZip({{"keep.toml", "keep\n"}, {"edit.toml", "old\n"}, {"drop.toml", "gone\n"}});
    auto ws = ZipWorkingSet::openFromBytes(blob);

    ws.writeEntry("edit.toml", asBytes("new\n"));
    ws.writeEntry("added.toml", asBytes("fresh\n"));
    ws.removeEntry("drop.toml");
    REQUIRE(ws.dirty());

    REQUIRE(asString(ws.read("edit.toml")->span()) == "new\n");
    REQUIRE(asString(ws.read("added.toml")->span()) == "fresh\n");
    REQUIRE_FALSE(ws.read("drop.toml").has_value());
    REQUIRE_FALSE(ws.contains("drop.toml"));
    REQUIRE(ws.contains("added.toml"));

    // Overrides and removals survive a serialize/reopen; the removed entry is
    // gone and the untouched one is intact.
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());
    REQUIRE(asString(out.read("keep.toml")->span()) == "keep\n");
    REQUIRE(asString(out.read("edit.toml")->span()) == "new\n");
    REQUIRE(asString(out.read("added.toml")->span()) == "fresh\n");
    REQUIRE_FALSE(out.contains("drop.toml"));
}

TEST_CASE("ZipWorkingSet synthesizes a directory tree from flat keys",
          "[viewer][zip_working_set]") {
    auto blob = makeZip(
        {{"top.toml", "t\n"}, {"a/b.toml", "b\n"}, {"a/c.toml", "c\n"}, {"a/sub/d.toml", "d\n"}});
    auto ws = ZipWorkingSet::openFromBytes(blob);

    auto root = ws.listAtPrefix("");
    REQUIRE(root.size() == 2);
    const auto *a = find(root, "a");
    REQUIRE(a != nullptr);
    REQUIRE(a->is_directory);
    REQUIRE(a->key == "a");
    const auto *top = find(root, "top.toml");
    REQUIRE(top != nullptr);
    REQUIRE_FALSE(top->is_directory);
    REQUIRE(top->bytes == 2); // "t\n"

    auto under_a = ws.listAtPrefix("a");
    REQUIRE(under_a.size() == 3);
    REQUIRE(find(under_a, "b.toml") != nullptr);
    REQUIRE(find(under_a, "c.toml") != nullptr);
    const auto *sub = find(under_a, "sub");
    REQUIRE(sub != nullptr);
    REQUIRE(sub->is_directory);
    REQUIRE(sub->key == "a/sub");

    // A write into a new subdirectory shows up in the synthesized tree; a
    // removal drops the leaf.
    ws.writeEntry("a/e.toml", asBytes("e\n"));
    ws.removeEntry("a/b.toml");
    auto under_a2 = ws.listAtPrefix("a");
    REQUIRE(find(under_a2, "e.toml") != nullptr);
    REQUIRE(find(under_a2, "b.toml") == nullptr);
}

TEST_CASE("ZipWorkingSet rejects a non-ZIP blob", "[viewer][zip_working_set]") {
    auto garbage = asBytes("this is definitely not a zip archive");
    REQUIRE_THROWS_AS(ZipWorkingSet::openFromBytes(garbage), std::runtime_error);
}

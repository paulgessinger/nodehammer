// `cli::run`, reached the way an installed consumer reaches it.
//
// The behaviour of the command line is covered in tests/cli/, against the
// static archive. What is asked here is the other question, and it is a
// question about *linkage*: is the entry point actually exported from
// libnodehammer, and does it work when the caller has only the public headers?
//
// This target links nodehammer_shared and nothing else, so an entry point
// missing its NH_API — or one whose namespace ci/check_shared_exports.py
// decided was internal — fails here and nowhere else in the tree. That is the
// whole reason the file is short: everything it could assert about behaviour is
// asserted next door, and duplicating it would blur what a failure here means.
//
// It is also the shape the next bookmark depends on. The Python extension links
// this same shared library through this same header set, so a `cli::run` that
// links here links there.

#include <catch2/catch_test_macros.hpp>

#include <nodehammer/cli.hpp>
#include <nodehammer/version.hpp>

#include <string_view>
#include <vector>

TEST_CASE("the command line is reachable through the shared library", "[public][cli]") {
    const std::vector<std::string_view> version{"--version"};
    CHECK(nodehammer::cli::run(version) == 0);

    // A failing command, and then a line after it. Through the archive that
    // pairing is a regression test; through the shared library it is the
    // promise the wheel is built on -- `std::exit` inside libnodehammer would
    // take the interpreter with it, and no Python-side care could prevent that.
    const std::vector<std::string_view> failing{"convert", "--input", "no-such-file.gdml",
                                                "--output", "out.glb"};
    CHECK(nodehammer::cli::run(failing) != 0);

    CHECK(nodehammer::cli::run(version) == 0);
}

TEST_CASE("the run options are part of the published surface", "[public][cli]") {
    // Defaults are API: an embedder that constructs `RunOptions{}` and gets a
    // pager has been handed a surprise it cannot see in its own code.
    const nodehammer::cli::RunOptions options;
    CHECK(options.pager == false);

    const std::vector<std::string_view> version{"--version"};
    CHECK(nodehammer::cli::run(version, options) == 0);
}

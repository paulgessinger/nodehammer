// The command line, exercised in process.
//
// There was no coverage of the CLI at all before this: tests/integration/ was
// empty, no `add_test` named the executable, and the only things that ran it
// were two developer recipes in the Justfile with no assertions. That was
// survivable while every command ended in `std::exit`, because there was
// nothing a test could have asserted afterwards -- the test binary would have
// exited too.
//
// Which is exactly what these cases are here to hold: `cli::run` *returns*.
// Every one of them runs a command that fails and then keeps running, and the
// suite reaching its final assertion is as much the result as the codes are.
//
// Deliberately in process rather than over a spawned binary. A subprocess would
// test the executable; this tests the entry point the executable and the Python
// bindings both go through, which is the thing that has two callers and
// therefore the thing that can regress for one of them.
//
// Where the line with tests/python/test_cli.py falls: what stays here is what
// has to hold on *every* platform and on *both* linkages -- the
// survives-a-failure pairing, `--version` parity, help on no arguments, the
// pager default. NODEHAMMER_BUILD_PYTHON is off by default and CI runs the
// Python legs on three of the matrix with Windows deliberately excluded, which
// is exactly the platform whose CLI differs most (_dup, _popen, the console
// subsystem). And pytest can only ever reach the shared library, while these
// cases link the archive.
//
// Breadth lives there instead: per-command behaviour and a parametrized sweep
// over the converted `std::exit` sites, which `capfd` and `parametrize` say far
// better than this file's hand-rolled capture could.
//
// Neither reaches `viewer`, whose fifteen converted sites are registered by the
// executable rather than by `cli::run`. Those are covered over the built binary
// from tests/CMakeLists.txt, which is the only place they are reachable at all.

#include "cli_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nodehammer/cli.hpp>
#include <nodehammer/version.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

using nhtest::currentProcessId;
using nhtest::Outcome;
using nhtest::runCaptured;

TEST_CASE("a failing command returns, and the caller is still running", "[cli]") {
    // The regression this whole refactor exists to prevent. Under the old CLI
    // this line ended the test binary with status 1 and Catch2 reported
    // nothing; the assertions after it are the point.
    const auto outcome =
        runCaptured({"convert", "--input", "no-such-file.gdml", "--output", "out.glb"});

    CHECK(outcome.code != 0);
    CHECK(outcome.mentions("no-such-file.gdml"));

    // Still here. Twice, to show the entry point is re-entrant and holds no
    // state between calls that a second one would trip over.
    const auto again =
        runCaptured({"convert", "--input", "no-such-file.gdml", "--output", "out.glb"});
    CHECK(again.code == outcome.code);
}

TEST_CASE("the version flag answers with the library's own version", "[cli]") {
    const auto outcome = runCaptured({"--version"});

    CHECK(outcome.code == 0);
    // Not a second copy of the string: the flag formats `nodehammer::VERSION`,
    // and this asserts they are the same one. The Python side pins
    // `__version__` to the same constant, so `nodehammer --version` from a
    // wheel and `nodehammer.__version__` cannot drift apart.
    CHECK(outcome.mentions(nodehammer::VERSION));
}

TEST_CASE("no arguments prints help and succeeds", "[cli]") {
    // The library answer, which is not the executable's: a bare `nodehammer`
    // opens the viewer when one is compiled in, and that default lives in
    // main.cpp precisely because a library caller must not inherit it.
    const auto outcome = runCaptured({});

    CHECK(outcome.code == 0);
    CHECK(outcome.mentions("SUBCOMMAND"));
    // The usage line specifically, not just the word anywhere in the help: the
    // description contains "nodehammer" too, so a looser check passed while
    // this path was printing "Usage: [OPTIONS] SUBCOMMAND" -- help that names
    // no program to type. This is the assertion that catches an unset app name.
    CHECK(outcome.mentions("nodehammer [OPTIONS]"));
}

TEST_CASE("the help flag succeeds and an unknown subcommand does not", "[cli]") {
    CHECK(runCaptured({"--help"}).code == 0);

    const auto unknown = runCaptured({"no-such-command"});
    CHECK(unknown.code != 0);
}

TEST_CASE("the pager is off unless the caller asks", "[cli]") {
    // Not observable directly -- the pager only engages on a TTY, and a test
    // runner has none. What is asserted is the shape: the option exists, both
    // values are accepted, and neither changes the answer.
    nodehammer::cli::RunOptions paged;
    paged.pager = true;

    CHECK(runCaptured({"--version"}, paged).code == 0);
    CHECK(runCaptured({"--version"}).code == 0);
    CHECK(nodehammer::cli::RunOptions{}.pager == false);
}

namespace {

/// A path in the temp directory that removes itself.
///
/// `convert` requires an `--output`, so a case about what it *says* still needs
/// somewhere for it to put what it makes.
struct ScratchFile {
    ScratchFile(std::string_view stem, std::string_view ext)
        : path{(std::filesystem::temp_directory_path() /
                std::format("{}_{}{}", stem,
                            std::chrono::steady_clock::now().time_since_epoch().count(), ext))
                   .string()} {}
    ~ScratchFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ScratchFile(const ScratchFile &) = delete;
    ScratchFile &operator=(const ScratchFile &) = delete;

    std::string path;
};

} // namespace

TEST_CASE("narration is off for a library caller and switchable from the arguments", "[cli]") {
    // The mirror of the pager case above, and the reason both defaults sit in
    // `RunOptions` rather than in the commands: the front door decides what
    // kind of caller this is, once, and every command inherits the answer.
    //
    // `--synthetic-box` so the case needs no fixture and no importer backend;
    // the summary line it prints is the commentary being switched.
    CHECK(nodehammer::cli::RunOptions{}.quiet == true);

    const ScratchFile target{"nh_run_quiet", ".nhb"};

    const auto silent = runCaptured({"convert", "--synthetic-box", "--output", target.path});
    CHECK(silent.code == 0);
    CHECK(silent.err.find("Nodes:") == std::string::npos);
    // Silent about the work, and silent on stdout in every posture: the answer
    // is the file.
    CHECK(silent.out.empty());

    // `-v` reaches the same switch from the argument list, for a caller that
    // does not own the `RunOptions` -- a harness, or somebody debugging one.
    const auto asked = runCaptured({"-v", "convert", "--synthetic-box", "--output", target.path});
    CHECK(asked.code == 0);
    CHECK(asked.err.find("Nodes:") != std::string::npos);
    CHECK(asked.out.empty());

    // And after the subcommand as well as before it, which is where people
    // actually type a global flag. CLI11 copies fallthrough into every
    // subcommand it constructs, so this holds two levels down as well.
    const auto trailing =
        runCaptured({"convert", "--synthetic-box", "--output", target.path, "-v"});
    CHECK(trailing.code == 0);
    CHECK(trailing.err.find("Nodes:") != std::string::npos);
}

TEST_CASE("a diagnostic is not narration, and no switch hides one", "[cli]") {
    // The line between the two, asserted: `-q` silences the account of work
    // going well. A command that could not do its job still says so, on stderr,
    // and still answers non-zero -- which is what makes the switch safe to
    // leave on by default for every caller that is a program.
    const auto outcome =
        runCaptured({"convert", "--input", "no-such-file.gdml", "--output", "out.glb"});

    CHECK(outcome.code != 0);
    CHECK(outcome.err.find("no-such-file.gdml") != std::string::npos);
}

// Native-only: `project` is not registered under Emscripten (there is no host
// to serve from), so over there this would be asserting on an unknown
// subcommand rather than on an unreadable archive.
#ifndef __EMSCRIPTEN__
TEST_CASE("project info on a file that is not an archive reports, and returns", "[cli][project]") {
    // `ZipWorkingSet::openFromFile` throws `std::runtime_error`, which the
    // command's reporting layer does not catch: this line used to end the
    // process -- and with it this test binary -- on any regular file that is
    // not a ZIP. That it *returns* is the assertion, as in the first case in
    // this file; what it says is the one after.
    const ScratchFile target{"nh_not_an_archive", ".nhproj"};
    {
        std::ofstream out(target.path, std::ios::binary);
        out << "this is not a ZIP archive\n";
    }

    const auto outcome = runCaptured({"project", "info", target.path});
    CHECK(outcome.code != 0);
    CHECK(outcome.err.find("archive") != std::string::npos);
}
#endif

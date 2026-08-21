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
// Where the line with pytest will fall, once `nodehammer.cli.run` exists: what
// stays here is what has to hold on *every* platform and on *both* linkages --
// the survives-a-failure pairing, `--version` parity, help on no arguments.
// NODEHAMMER_BUILD_PYTHON is off by default and CI runs the Python legs on
// three of the matrix with Windows deliberately excluded, which is exactly the
// platform whose CLI differs most (_dup, _popen, the console). And pytest can
// only ever reach the shared library, while these cases link the archive.
//
// What moves there is breadth: per-command behaviour, and a parametrized sweep
// over the converted `std::exit` sites, which `capfd` and `parametrize` express
// far better than this file's hand-rolled capture does. The two cases below
// marked as such are the ones to take.

#include <catch2/catch_test_macros.hpp>

#include <nodehammer/cli.hpp>
#include <nodehammer/version.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
int duplicateFd(int fd) { return _dup(fd); }
int duplicateTo(int source, int target) { return _dup2(source, target); }
int closeFd(int fd) { return _close(fd); }
int fileNo(std::FILE *f) { return _fileno(f); }
long currentProcessId() { return static_cast<long>(_getpid()); }
#else
int duplicateFd(int fd) { return dup(fd); }
int duplicateTo(int source, int target) { return dup2(source, target); }
int closeFd(int fd) { return close(fd); }
int fileNo(std::FILE *f) { return fileno(f); }
long currentProcessId() { return static_cast<long>(getpid()); }
#endif

/// Everything a command wrote, and the code it answered with.
struct Outcome {
    int code = 0;
    std::string out;
    std::string err;

    [[nodiscard]] bool mentions(std::string_view text) const {
        return out.find(text) != std::string::npos || err.find(text) != std::string::npos;
    }
};

/// Run one command line with stdout and stderr captured.
///
/// The same `dup`/`dup2` dance `Pager` performs, for the opposite reason: the
/// commands print through `std::print` and `std::println` to the C streams, so
/// a test that wants to read what they said has to redirect the descriptor
/// rather than a C++ stream object. Restoring in every path matters -- a failed
/// assertion here would otherwise take the rest of the suite's output with it.
Outcome runCaptured(std::vector<std::string_view> args,
                    const nodehammer::cli::RunOptions &options = {}) {
    // Named per process, not per suite: catch_discover_tests registers every
    // TEST_CASE as its own ctest entry, so `ctest -j` runs several of these
    // binaries at once and a fixed name would have them truncating each other's
    // capture file.
    const auto dir = std::filesystem::temp_directory_path();
    const auto tag = std::to_string(static_cast<long long>(currentProcessId()));
    const auto outPath = dir / ("nh_cli_test_out_" + tag + ".txt");
    const auto errPath = dir / ("nh_cli_test_err_" + tag + ".txt");

    std::FILE *outFile = std::fopen(outPath.string().c_str(), "w+");
    std::FILE *errFile = std::fopen(errPath.string().c_str(), "w+");
    REQUIRE(outFile != nullptr);
    REQUIRE(errFile != nullptr);

    const int savedOut = duplicateFd(fileNo(stdout));
    const int savedErr = duplicateFd(fileNo(stderr));
    std::fflush(stdout);
    std::fflush(stderr);
    duplicateTo(fileNo(outFile), fileNo(stdout));
    duplicateTo(fileNo(errFile), fileNo(stderr));

    Outcome outcome;
    outcome.code = nodehammer::cli::run(args, options);

    std::fflush(stdout);
    std::fflush(stderr);
    duplicateTo(savedOut, fileNo(stdout));
    duplicateTo(savedErr, fileNo(stderr));
    closeFd(savedOut);
    closeFd(savedErr);
    std::fclose(outFile);
    std::fclose(errFile);

    const auto slurp = [](const std::filesystem::path &path) {
        std::string text;
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr) {
            return text;
        }
        std::array<char, 4096> buf{};
        while (const auto n = std::fread(buf.data(), 1, buf.size(), f)) {
            text.append(buf.data(), n);
        }
        std::fclose(f);
        return text;
    };
    outcome.out = slurp(outPath);
    outcome.err = slurp(errPath);

    std::filesystem::remove(outPath);
    std::filesystem::remove(errPath);
    return outcome;
}

} // namespace

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

TEST_CASE("a usage error reports its code and returns one", "[cli]") {
    // Moves to pytest with the rest of the per-command breadth. Here for now
    // because it is the only coverage these sites have.
    //
    // One of the fifteen `std::exit(1)` calls that used to live in
    // cmd_viewer.cpp -- chosen because it fails before any window is
    // constructed, so it is the same check on a headless machine.
    const auto outcome = runCaptured({"viewer", "--camera-distance", "5"});

    if (outcome.mentions("no such subcommand") || outcome.code == 106) {
        // Built without the native viewer: the subcommand does not exist, which
        // is a different (and correct) failure. Nothing to assert about NH0900.
        CHECK(outcome.code != 0);
        return;
    }
    CHECK(outcome.code == 1);
    CHECK(outcome.mentions("NH0900"));
}

TEST_CASE("a config that cannot be written reports it and returns one", "[cli]") {
    // Moves to pytest, as above.
    const auto missing = std::filesystem::path{"no-such-directory"} / "out.toml";
    const auto outcome = runCaptured(
        {"config-flatten", "--config", "no-such-config.toml", "--output", missing.string()});

    CHECK(outcome.code == 1);
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

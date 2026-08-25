#pragma once

// Running `cli::run` in process and reading what it said.
//
// Extracted from test_cli_run.cpp when a second suite needed it. The capture is
// at the file-descriptor level rather than over a C++ stream object because the
// commands print through `std::print`/`std::println` to the C streams, so
// redirecting `std::cout` would see nothing.
//
// In a header with everything `inline`, and not a .cpp: it is a handful of
// short functions over one struct, and a translation unit of its own would need
// its own entry in tests/CMakeLists.txt for no gain.

#include <nodehammer/cli.hpp>

#include <catch2/catch_test_macros.hpp>

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

namespace nhtest {

#ifdef _WIN32
inline int duplicateFd(int fd) { return _dup(fd); }
inline int duplicateTo(int source, int target) { return _dup2(source, target); }
inline int closeFd(int fd) { return _close(fd); }
inline int fileNo(std::FILE *f) { return _fileno(f); }
inline long currentProcessId() { return static_cast<long>(_getpid()); }
#else
inline int duplicateFd(int fd) { return dup(fd); }
inline int duplicateTo(int source, int target) { return dup2(source, target); }
inline int closeFd(int fd) { return close(fd); }
inline int fileNo(std::FILE *f) { return fileno(f); }
inline long currentProcessId() { return static_cast<long>(getpid()); }
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
inline Outcome runCaptured(std::vector<std::string_view> args,
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

} // namespace nhtest

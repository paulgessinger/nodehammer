#include "cli_test_support.hpp"
#include <cli/import_log_guard.hpp>
#include <diagnostic_codes.hpp>
#include <iostream>
#include <stdexcept>

namespace {
// Capture both descriptors without involving an importer or optional dependency.
class Capture {
  public:
    Capture() {
        std::cout.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        out = std::tmpfile();
        err = std::tmpfile();
        savedOut = nhtest::duplicateFd(nhtest::fileNo(stdout));
        savedErr = nhtest::duplicateFd(nhtest::fileNo(stderr));
        if (!out || !err || savedOut < 0 || savedErr < 0 ||
            nhtest::duplicateTo(nhtest::fileNo(out), nhtest::fileNo(stdout)) < 0 ||
            nhtest::duplicateTo(nhtest::fileNo(err), nhtest::fileNo(stderr)) < 0) {
            restore();
            throw std::runtime_error("cannot capture descriptors");
        }
    }
    ~Capture() { restore(); }
    std::pair<std::string, std::string> finish() {
        std::cout.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        auto read = [](FILE *file) {
            std::rewind(file);
            std::string result;
            char buffer[256];
            while (auto n = std::fread(buffer, 1, sizeof(buffer), file))
                result.append(buffer, n);
            return result;
        };
        auto result = std::pair{read(out), read(err)};
        restore();
        return result;
    }

  private:
    void restore() noexcept {
        if (savedOut >= 0) {
            nhtest::duplicateTo(savedOut, nhtest::fileNo(stdout));
            nhtest::closeFd(savedOut);
            savedOut = -1;
        }
        if (savedErr >= 0) {
            nhtest::duplicateTo(savedErr, nhtest::fileNo(stderr));
            nhtest::closeFd(savedErr);
            savedErr = -1;
        }
        if (out) {
            std::fclose(out);
            out = nullptr;
        }
        if (err) {
            std::fclose(err);
            err = nullptr;
        }
    }
    FILE *out = nullptr, *err = nullptr;
    int savedOut = -1, savedErr = -1;
};
} // namespace

TEST_CASE("native import log guard redirects C and C++ output and restores on unwind",
          "[cli][import-log]") {
    using nodehammer::cli::detail::ImportLogGuard;
    Capture capture;
    std::cout << "before;";
    {
        const ImportLogGuard guard{nodehammer::codes::kFatalGeoModelRead, "redirect failed"};
        std::printf("C-import;");
        std::cout << "C++-import;";
    }
    std::printf("after;");
    try {
        const ImportLogGuard guard{nodehammer::codes::kFatalTgeoOpenFailed, "redirect failed"};
        std::cout << "throwing-import;";
        throw std::runtime_error("import failed");
    } catch (const std::runtime_error &) {
        std::cout << "restored;";
    }
    const auto [out, err] = capture.finish();
    REQUIRE(out == "before;after;restored;");
    REQUIRE(err == "C-import;C++-import;throwing-import;");
}

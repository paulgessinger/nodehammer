// The ladder that finds the wasm runtime, and the check that refuses one it
// cannot serve.
//
// Everything here is about *rejection*, because that is where the value is. A
// runtime that resolves shows up the moment anyone serves it; a runtime that
// resolves and is wrong shows up as a blank page, or -- the case that actually
// happened -- a compute worker that sits at 0% forever because its postMessage
// keys were renamed on the other side of a Closure build (dcc4a06). No wheel
// pin can see that, so this check is the only thing that can, and an untested
// check here is an unchecked one.
//
// The directories are synthesised rather than taken from a build tree: the
// point is to exercise the shapes a person can produce by hand, and a real
// runtime is 7 MB of bundles that say nothing about any of them.

#include "web/runtime_locator.hpp"

#include "diagnostic_codes.hpp"

#include <nodehammer/diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace web = nodehammer::web;

namespace {

/// A directory that goes away when the test does.
class TempDir {
  public:
    TempDir() : path_(fs::temp_directory_path() / uniqueName()) { fs::create_directories(path_); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const fs::path &path() const { return path_; }

    void write(std::string_view name, std::string_view content) const {
        std::ofstream out(path_ / name, std::ios::binary);
        out << content;
    }

  private:
    static std::string uniqueName() {
        // No getpid(): it is POSIX-only, and the suite runs on Windows and
        // under node. A steady tick plus a counter is unique enough for a
        // directory that lives for one test case.
        static int n = 0;
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::format("nh_runtime_test_{}_{}", tick, n++);
    }
    fs::path path_;
};

/// A directory that passes every check: the stamp *and* the payload.
void makeGoodRuntime(const TempDir &dir) {
    for (const auto *name : {"index.html", "compute_worker.js", "nodehammer-gles3.js",
                             "nodehammer-gles3.wasm", "nodehammer-wgpu.js", "nodehammer-wgpu.wasm",
                             "nodehammer-compute.js", "nodehammer-compute.wasm"}) {
        dir.write(name, "x");
    }
    dir.write("nh_runtime.json", std::format("{{\"schema\": {}, \"version\": \"9.9.9-test\"}}",
                                             web::compiledSchema()));
}

/// Sets (or clears, with nullopt) an environment variable for one test case and
/// puts it back. Clearing matters as much as setting: a developer who has
/// NODEHAMMER_WEB_ASSETS exported would otherwise change what the no-argument
/// cases below are testing.
class ScopedEnv {
  public:
    ScopedEnv(const char *name, const std::optional<std::string> &value) : name_(name) {
        if (const char *old = std::getenv(name)) {
            previous_ = old;
        }
        apply(value);
    }
    ~ScopedEnv() { apply(previous_); }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    void apply(const std::optional<std::string> &value) {
#ifdef _WIN32
        _putenv_s(name_, value ? value->c_str() : "");
#else
        if (value) {
            ::setenv(name_, value->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
#endif
    }
    const char *name_;
    std::optional<std::string> previous_;
};

} // namespace

TEST_CASE("an explicit directory that is a runtime answers the ladder", "[web][runtime]") {
    TempDir dir;
    makeGoodRuntime(dir);

    const web::RuntimeLocation loc = web::locateRuntime({.explicitDir = dir.path()});
    CHECK(loc.dir == dir.path());
    CHECK(loc.schema == web::compiledSchema());
    CHECK(loc.version == "9.9.9-test");
}

TEST_CASE("a schema the library does not serve is refused, not served", "[web][runtime]") {
    // The case no version pin can reach: same project, same version, a runtime
    // built against a different contract. This is the whole reason the stamp
    // carries an integer rather than only a version string.
    TempDir dir;
    dir.write("index.html", "<!doctype html>");
    dir.write("nh_runtime.json", std::format("{{\"schema\": {}, \"version\": \"9.9.9-test\"}}",
                                             web::compiledSchema() + 1));

    try {
        (void)web::locateRuntime({.explicitDir = dir.path()});
        FAIL("a mismatched runtime was accepted");
    } catch (const nodehammer::Error &e) {
        CHECK(e.code() == nodehammer::codes::kFatalWebRuntimeSchema);
        // The refusal has to name both ids and the directory, or the reader
        // cannot tell which of the two halves to move. The directory rides in
        // the context and the ids in the explanation, because `Error::what()` is
        // echoed twice by the CLI's reporter and so stays one sentence.
        CHECK(e.context() == dir.path().string());
        const std::string explained =
            web::explainLadder(web::walkLadder({.explicitDir = dir.path()}));
        CHECK_THAT(explained, Catch::Matchers::ContainsSubstring(dir.path().string()));
        CHECK_THAT(explained,
                   Catch::Matchers::ContainsSubstring(std::to_string(web::compiledSchema() + 1)));
        CHECK_THAT(explained,
                   Catch::Matchers::ContainsSubstring(std::to_string(web::compiledSchema())));
    }
}

TEST_CASE("a stamp is not a payload", "[web][runtime]") {
    // The bug this exists for was real and silent. The wasm *build tree* carries
    // a stamp and the bundles but neither the shell nor the worker script --
    // those two are only ever installed -- so a schema-only check accepted it and
    // the server then answered 404 on `/`. That is precisely the blank page the
    // stamp is supposed to prevent, arriving through the stamp check itself.
    TempDir dir;
    dir.write("nh_runtime.json", std::format("{{\"schema\": {}, \"version\": \"9.9.9-test\"}}",
                                             web::compiledSchema()));
    dir.write("nodehammer-gles3.js", "x");

    try {
        (void)web::locateRuntime({.explicitDir = dir.path()});
        FAIL("a stamped but incomplete runtime was accepted");
    } catch (const nodehammer::Error &e) {
        CHECK(e.code() == nodehammer::codes::kFatalWebRuntimeNotFound);
        const std::vector<web::RuntimeCandidate> ladder =
            web::walkLadder({.explicitDir = dir.path()});
        REQUIRE(ladder.size() == 1);
        // Names what is absent, not merely that something is.
        CHECK_THAT(ladder.front().rejection, Catch::Matchers::ContainsSubstring("incomplete"));
        CHECK_THAT(ladder.front().rejection, Catch::Matchers::ContainsSubstring("index.html"));
        CHECK_THAT(ladder.front().rejection,
                   Catch::Matchers::ContainsSubstring("compute_worker.js"));
    }
}

TEST_CASE("the explanation says what to do, not only what failed", "[web][runtime]") {
    // A native build legitimately has no runtime, so this message is the primary
    // user-facing behaviour of `--web` for anyone building from source. If it
    // reads as "broken build", the feature reads as broken.
    const ScopedEnv clear("NODEHAMMER_WEB_ASSETS", std::nullopt);
    const std::string text = web::explainLadder(web::walkLadder());

    CHECK_THAT(text, Catch::Matchers::ContainsSubstring("--web-assets"));
    CHECK_THAT(text, Catch::Matchers::ContainsSubstring("NODEHAMMER_WEB_ASSETS"));
    CHECK_THAT(text, Catch::Matchers::ContainsSubstring("separate Emscripten"));
}

TEST_CASE("a directory that is not a runtime says so, and says which", "[web][runtime]") {
    TempDir dir;
    try {
        (void)web::locateRuntime({.explicitDir = dir.path()});
        FAIL("an empty directory was accepted");
    } catch (const nodehammer::Error &e) {
        CHECK(e.code() == nodehammer::codes::kFatalWebRuntimeNotFound);
        CHECK(e.context() == dir.path().string());
        const std::string explained =
            web::explainLadder(web::walkLadder({.explicitDir = dir.path()}));
        CHECK_THAT(explained, Catch::Matchers::ContainsSubstring(dir.path().string()));
        CHECK_THAT(explained, Catch::Matchers::ContainsSubstring("nh_runtime.json"));
    }
}

TEST_CASE("a runtime older than the stamp is diagnosed as old, not as absent", "[web][runtime]") {
    // A shell with no stamp is a real runtime from before the stamp existed, or
    // one somebody assembled by hand. Calling that "not a nodehammer web
    // runtime" would send the reader looking in the wrong place.
    TempDir dir;
    dir.write("index.html", "<!doctype html>");

    try {
        (void)web::locateRuntime({.explicitDir = dir.path()});
        FAIL("a stampless runtime was accepted");
    } catch (const nodehammer::Error &e) {
        CHECK(e.code() == nodehammer::codes::kFatalWebRuntimeNotFound);
        CHECK_THAT(web::explainLadder(web::walkLadder({.explicitDir = dir.path()})),
                   Catch::Matchers::ContainsSubstring("no nh_runtime.json"));
    }
}

TEST_CASE("a malformed stamp is refused rather than half-read", "[web][runtime]") {
    TempDir dir;
    dir.write("index.html", "<!doctype html>");

    SECTION("not JSON at all") {
        dir.write("nh_runtime.json", "this is not json");
        CHECK_THROWS_AS(web::locateRuntime({.explicitDir = dir.path()}), nodehammer::Error);
    }
    SECTION("JSON without a schema") {
        dir.write("nh_runtime.json", "{\"version\": \"1.0\"}");
        CHECK_THROWS_AS(web::locateRuntime({.explicitDir = dir.path()}), nodehammer::Error);
    }
    SECTION("a schema that is not an integer") {
        dir.write("nh_runtime.json", "{\"schema\": \"1\", \"version\": \"1.0\"}");
        CHECK_THROWS_AS(web::locateRuntime({.explicitDir = dir.path()}), nodehammer::Error);
    }
    SECTION("no version") {
        dir.write("nh_runtime.json", std::format("{{\"schema\": {}}}", web::compiledSchema()));
        CHECK_THROWS_AS(web::locateRuntime({.explicitDir = dir.path()}), nodehammer::Error);
    }
}

TEST_CASE("the environment variable is a rung, below an explicit path", "[web][runtime]") {
    // The escape hatch: it has to outrank anything automatic (the sibling
    // package, the install tree) so a locally built runtime can be tried
    // without uninstalling anything, and be outranked by the flag so a command
    // line still means what it says. Both halves are asserted here, because an
    // ordering is worth nothing if only one end of it is checked.
    TempDir fromEnv;
    makeGoodRuntime(fromEnv);
    ScopedEnv env("NODEHAMMER_WEB_ASSETS", fromEnv.path().string());

    SECTION("it answers when nothing more explicit was given") {
        const web::RuntimeLocation loc = web::locateRuntime();
        CHECK(loc.dir == fromEnv.path());
    }

    SECTION("an explicit path outranks it") {
        TempDir explicitDir;
        makeGoodRuntime(explicitDir);
        const web::RuntimeLocation loc = web::locateRuntime({.explicitDir = explicitDir.path()});
        CHECK(loc.dir == explicitDir.path());
        CHECK(loc.dir != fromEnv.path());
    }

    SECTION("and a bad one is an error, not a reason to keep looking") {
        ScopedEnv bad("NODEHAMMER_WEB_ASSETS", (fromEnv.path() / "nope").string());
        const std::vector<web::RuntimeCandidate> ladder = web::walkLadder();
        REQUIRE(ladder.size() == 1);
        CHECK(ladder.front().rung == web::RuntimeRung::Environment);
        CHECK_THROWS_AS(web::locateRuntime(), nodehammer::Error);
    }
}

TEST_CASE("the embedder rung sits between the environment and the install tree", "[web][runtime]") {
    // The rung a wheel fills: `nodehammer-web` installs the runtime into
    // site-packages, which nothing below could guess, and `nodehammer.cli.run`
    // hands the path down through RunOptions rather than the library going
    // looking for an interpreter.
    //
    // Its *position* is the whole design and is what is asserted here. It is an
    // automatic default, so a person must be able to override it; it is a
    // statement about a real installed package, so a broken one must not fall
    // through to a guess and serve something else.
    const ScopedEnv clear("NODEHAMMER_WEB_ASSETS", std::nullopt);

    TempDir fromPackage;
    makeGoodRuntime(fromPackage);

    SECTION("it answers when nothing more explicit was given") {
        const web::RuntimeLocation loc = web::locateRuntime({.embedderDir = fromPackage.path()});
        CHECK(loc.dir == fromPackage.path());
    }

    SECTION("an explicit path outranks it") {
        TempDir explicitDir;
        makeGoodRuntime(explicitDir);
        const web::RuntimeLocation loc = web::locateRuntime(
            {.explicitDir = explicitDir.path(), .embedderDir = fromPackage.path()});
        CHECK(loc.dir == explicitDir.path());
    }

    SECTION("so does the environment variable") {
        TempDir fromEnv;
        makeGoodRuntime(fromEnv);
        const ScopedEnv env("NODEHAMMER_WEB_ASSETS", fromEnv.path().string());
        const web::RuntimeLocation loc = web::locateRuntime({.embedderDir = fromPackage.path()});
        CHECK(loc.dir == fromEnv.path());
    }

    SECTION("and a broken one is an error, not a reason to keep looking") {
        const std::vector<web::RuntimeCandidate> ladder =
            web::walkLadder({.embedderDir = fromPackage.path() / "nope"});
        REQUIRE(ladder.size() == 1);
        CHECK(ladder.front().rung == web::RuntimeRung::Embedder);
        CHECK_THROWS_AS(web::locateRuntime({.embedderDir = fromPackage.path() / "nope"}),
                        nodehammer::Error);
    }
}

TEST_CASE("the install-tree rung is relative to the executable, not the cwd", "[web][runtime]") {
    // Rung 3 cannot be resolved in a test -- a test binary is not an install
    // tree -- but the path it *builds* is the whole of what this rung is, and
    // getting it from the executable rather than the working directory is the
    // reason for the platform shim.
#ifndef __EMSCRIPTEN__
    const ScopedEnv clear("NODEHAMMER_WEB_ASSETS", std::nullopt);

    const auto exe = web::executablePath();
    REQUIRE(exe.has_value());

    const std::vector<web::RuntimeCandidate> ladder = web::walkLadder();
    REQUIRE_FALSE(ladder.empty());
    const web::RuntimeCandidate &rung3 = ladder.back();
    REQUIRE(rung3.rung == web::RuntimeRung::InstallTree);
    CHECK(rung3.path == exe->parent_path().parent_path() / "share" / "nodehammer" / "web");
#endif
}

TEST_CASE("a stated rung that does not answer stops the walk", "[web][runtime]") {
    // Precedence is not "first one that works" but "first one that was stated".
    // Falling through from a wrong --web-assets to whatever happens to be
    // installed would serve something other than what was asked for, and say
    // nothing about having done so.
    TempDir dir;
    const std::vector<web::RuntimeCandidate> ladder =
        web::walkLadder({.explicitDir = dir.path() / "nope"});

    REQUIRE(ladder.size() == 1);
    CHECK(ladder.front().rung == web::RuntimeRung::Explicit);
    CHECK_FALSE(ladder.front().rejection.empty());
}

TEST_CASE("the ladder reports every rung it walked", "[web][runtime]") {
    // With no explicit directory and (in the test environment) no environment
    // variable, the walk reaches the install-tree rung and stops there. What
    // matters is that the failure carries the list -- a from-source build has
    // no wasm anywhere, and that is the *normal* state, so a bare "not found"
    // would read as a bug in the build the reader just made.
    const ScopedEnv clear("NODEHAMMER_WEB_ASSETS", std::nullopt);

    const std::vector<web::RuntimeCandidate> ladder = web::walkLadder();
    REQUIRE_FALSE(ladder.empty());
    CHECK(ladder.back().rung == web::RuntimeRung::InstallTree);
}

TEST_CASE("the executable path is this executable, where the platform has one", "[web][runtime]") {
#ifdef __EMSCRIPTEN__
    // A wasm module has no executable path. One rung unavailable, not an error.
    CHECK_FALSE(web::executablePath().has_value());
#else
    const auto exe = web::executablePath();
    REQUIRE(exe.has_value());
    CHECK(exe->is_absolute());
    CHECK(std::filesystem::exists(*exe));
    // Not argv[0], and not a directory: this is the running binary itself.
    CHECK(exe->filename().string().find("nodehammer_tests") != std::string::npos);
#endif
}

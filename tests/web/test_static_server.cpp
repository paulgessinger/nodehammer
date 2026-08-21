// The server behind `viewer --web`.
//
// The claim worth testing is not "HTTP works" -- cpp-httplib's own suite covers
// that -- but that *this* configuration serves *this* payload correctly. Three
// things in it are ours and would each fail silently:
//
//   - `Cache-Control: no-store`, without which a rebuilt compute worker is
//     served stale from inside a Web Worker, where a hard reload's cache bypass
//     does not reach (scripts/serve_nocache.py exists for this);
//   - `application/wasm`, without which a browser will not stream-compile and
//     buffers 2.8 MB instead, which looks like a hang rather than an error;
//   - `/` resolving to the shell, which is the whole reason it is named
//     index.html.
//
// Plus the property the plan asks for by name: every file in the staged root
// comes back 200. That is the cheapest proof the payload and the server agree,
// and it needs no browser.

#include "web/static_server.hpp"

#include <nodehammer/diagnostics.hpp>

#include <httplib.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace web = nodehammer::web;

namespace {

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
        const fs::path target = path_ / name;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << content;
    }

  private:
    static std::string uniqueName() {
        static int n = 0;
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::format("nh_server_test_{}_{}", tick, n++);
    }
    fs::path path_;
};

/// A root shaped like what `stageRoot` produces, small enough to assert on.
void makePayload(const TempDir &dir) {
    dir.write("index.html", "<!doctype html><title>shell</title>");
    dir.write("compute_worker.js", "// worker");
    dir.write("nodehammer-gles3.js", "// loader");
    dir.write("nodehammer-gles3.wasm", "\0asm fake");
    dir.write("nh_runtime.json", "{\"schema\": 1, \"version\": \"test\"}");
    dir.write("nh_manifest.json", "{\"archive\": \"p.nhproj\", \"lock\": true}");
    dir.write("p.nhproj", "PK fake archive");
}

httplib::Client clientFor(const web::ServerHandle &h) {
    httplib::Client cli("127.0.0.1", h.port());
    cli.set_read_timeout(std::chrono::seconds(10));
    return cli;
}

} // namespace

TEST_CASE("every file in the staged root comes back 200", "[web][server]") {
    TempDir dir;
    makePayload(dir);

    const web::ServerHandle h = web::serve({.root = dir.path()});
    REQUIRE(h.running());
    auto cli = clientFor(h);

    // Walked rather than listed, so a file added to the payload later is covered
    // without this test being edited -- the point is that the *root* is
    // reachable, not that a hand-written list is.
    int seen = 0;
    for (const auto &entry : fs::recursive_directory_iterator(dir.path())) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string key = fs::relative(entry.path(), dir.path()).generic_string();
        const auto res = cli.Get("/" + key);
        REQUIRE(res);
        CHECK(res->status == 200);
        CHECK(res->body.size() == fs::file_size(entry.path()));
        ++seen;
    }
    CHECK(seen == 7);
}

TEST_CASE("the root path resolves to the shell", "[web][server]") {
    // The entire reason the shell is index.html and not viewer.html: one mount
    // point, no special case, and every other static host agrees.
    TempDir dir;
    makePayload(dir);

    const web::ServerHandle h = web::serve({.root = dir.path()});
    const auto res = clientFor(h).Get("/");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK_THAT(res->body, Catch::Matchers::ContainsSubstring("<title>shell</title>"));
}

TEST_CASE("responses forbid caching, and wasm is typed for streaming", "[web][server]") {
    TempDir dir;
    makePayload(dir);

    const web::ServerHandle h = web::serve({.root = dir.path()});
    auto cli = clientFor(h);

    const auto shell = cli.Get("/index.html");
    REQUIRE(shell);
    CHECK(shell->get_header_value("Cache-Control") == "no-store");

    const auto wasm = cli.Get("/nodehammer-gles3.wasm");
    REQUIRE(wasm);
    CHECK(wasm->get_header_value("Content-Type") == "application/wasm");
    CHECK(wasm->get_header_value("Cache-Control") == "no-store");

    const auto archive = cli.Get("/p.nhproj");
    REQUIRE(archive);
    CHECK(archive->get_header_value("Content-Type") == "application/zip");
}

TEST_CASE("nothing above the root is reachable", "[web][server]") {
    TempDir outer;
    outer.write("secret.txt", "not yours");
    const fs::path root = outer.path() / "root";
    fs::create_directories(root);
    {
        std::ofstream(root / "index.html") << "shell";
    }

    const web::ServerHandle h = web::serve({.root = root});
    auto cli = clientFor(h);

    for (const std::string attempt :
         {"/../secret.txt", "/%2e%2e/secret.txt", "/a/../../secret.txt"}) {
        const auto res = cli.Get(attempt);
        REQUIRE(res);
        CHECK(res->status != 200);
    }
}

TEST_CASE("a missing file is 404, not a hang", "[web][server]") {
    TempDir dir;
    makePayload(dir);
    const web::ServerHandle h = web::serve({.root = dir.path()});
    const auto res = clientFor(h).Get("/nope.js");
    REQUIRE(res);
    CHECK(res->status == 404);
}

TEST_CASE("the port is known before serve returns", "[web][server]") {
    // The reason binding happens on the calling thread: a caller that prints the
    // URL, or hands it to a browser, must not race the accept loop for it.
    TempDir dir;
    makePayload(dir);

    const web::ServerHandle h = web::serve({.root = dir.path()});
    CHECK(h.port() != 0);
    CHECK(h.url() == std::format("http://127.0.0.1:{}", h.port()));
    // Reachable *now*, with no sleep anywhere in this test.
    const auto res = clientFor(h).Get("/");
    REQUIRE(res);
    CHECK(res->status == 200);
}

TEST_CASE("stopping is idempotent, and the destructor stops too", "[web][server]") {
    TempDir dir;
    makePayload(dir);

    {
        web::ServerHandle h = web::serve({.root = dir.path()});
        CHECK(h.running());
        h.stop();
        CHECK_FALSE(h.running());
        h.stop(); // twice, deliberately
        CHECK_FALSE(h.running());
    }

    // And a handle that was never stopped by hand: leaving the scope is enough.
    // If it were not, this test would hang rather than fail, which is why it is
    // worth its own case.
    unsigned short port = 0;
    {
        const web::ServerHandle h = web::serve({.root = dir.path()});
        port = h.port();
    }
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(std::chrono::seconds(2));
    CHECK_FALSE(static_cast<bool>(cli.Get("/")));
}

TEST_CASE("serving a directory that is not there is an error, not an empty server",
          "[web][server]") {
    TempDir dir;
    CHECK_THROWS_AS(web::serve({.root = dir.path() / "absent"}), nodehammer::Error);
}

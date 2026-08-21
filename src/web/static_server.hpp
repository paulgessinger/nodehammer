#pragma once

// A static file server for one staged directory.
//
// Small on purpose. It serves the ~10 files `stageRoot` assembles, over
// loopback, to one browser — which is a different problem from serving the
// internet, and a few hundred lines of it beats a dependency. The moment the
// routes stop being static (docs/cli-and-web-viewer-plan.md Part 4's
// `HttpProjectFs`) that trade flips and this should become cpp-httplib.
//
// **The handle is the design.** `serve` binds, starts a thread and returns; it
// never loops until interrupted. Two reasons, both about the Python door:
// `gui.show()` in a notebook has to return or the session is over, and — the
// decisive one — with the GIL released, SIGINT lands on the Python main thread
// while a C++ `accept()` loop never checks `PyErr_CheckSignals()`. A blocking
// `serve` would hang until SIGKILL. Non-blocking makes interruption Python's
// problem, which is where it is solvable. The CLI writes its own wait loop
// around the same handle, so blocking stays a property of the front door.

#include <filesystem>
#include <memory>
#include <string>

namespace nodehammer::web {

struct ServeOptions {
    /// The directory to serve. Everything under it, and nothing above it.
    std::filesystem::path root;

    /// Loopback by default. For the remote-machine case the honest answer is
    /// loopback plus SSH forwarding; binding wider is possible and warns.
    std::string host{"127.0.0.1"};

    /// 0 asks the OS for a free one, which `port()` then reports. That is the
    /// default because a fixed port collides with `just wasm-serve` and with a
    /// second copy of this command, and neither failure is interesting.
    unsigned short port{0};
};

/// A running server. Stops on `stop()`, and on destruction.
class ServerHandle {
  public:
    ServerHandle() noexcept;
    ~ServerHandle();

    ServerHandle(ServerHandle &&) noexcept;
    ServerHandle &operator=(ServerHandle &&) noexcept;
    ServerHandle(const ServerHandle &) = delete;
    ServerHandle &operator=(const ServerHandle &) = delete;

    [[nodiscard]] bool running() const noexcept;

    /// The port actually bound — the point of asking for 0.
    [[nodiscard]] unsigned short port() const noexcept;

    /// e.g. "http://127.0.0.1:53112". Empty when not running.
    [[nodiscard]] std::string url() const;

    /// Idempotent, and safe from any thread.
    void stop() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit ServerHandle(std::unique_ptr<Impl> impl) noexcept;
    friend ServerHandle serve(const ServeOptions &);
};

/// Bind, start serving, and return. Throws `Error` if the socket cannot be had.
[[nodiscard]] ServerHandle serve(const ServeOptions &options);

} // namespace nodehammer::web

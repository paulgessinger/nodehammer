#include "web/static_server.hpp"

#include "diagnostic_codes.hpp"

#include <nodehammer/diagnostics.hpp>

#include <httplib.h>

#include <atomic>
#include <format>
#include <thread>

namespace nodehammer::web {
namespace {

/// Types the payload actually contains, beyond what httplib already knows.
///
/// The `.wasm` entry is not cosmetic: a browser refuses to stream-compile
/// anything not served as application/wasm, and falls back to buffering the
/// whole module — which for 2.8 MB is the difference between the viewer coming
/// up and the viewer appearing to hang.
void teachMimeTypes(httplib::Server &server) {
    server.set_file_extension_and_mimetype_mapping("wasm", "application/wasm");
    server.set_file_extension_and_mimetype_mapping("nhproj", "application/zip");
    server.set_file_extension_and_mimetype_mapping("zst", "application/zstd");
    server.set_file_extension_and_mimetype_mapping("nhb", "application/octet-stream");
    server.set_file_extension_and_mimetype_mapping("toml", "text/plain");
}

} // namespace

struct ServerHandle::Impl {
    httplib::Server server;
    std::filesystem::path root;
    std::string host;
    int port{0};
    std::thread thread;
    std::atomic<bool> stopped{false};

    ~Impl() { shutdown(); }

    void shutdown() noexcept {
        // `stop()` is documented as callable from another thread, and is a no-op
        // on a server that is not listening -- so this stays idempotent without
        // a flag guarding the call itself.
        if (stopped.exchange(true)) {
            if (thread.joinable()) {
                thread.join();
            }
            return;
        }
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

ServerHandle::ServerHandle() noexcept = default;
ServerHandle::ServerHandle(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ServerHandle::ServerHandle(ServerHandle &&) noexcept = default;
ServerHandle &ServerHandle::operator=(ServerHandle &&) noexcept = default;
ServerHandle::~ServerHandle() = default;

bool ServerHandle::running() const noexcept {
    return impl_ && !impl_->stopped.load(std::memory_order_relaxed);
}

unsigned short ServerHandle::port() const noexcept {
    return impl_ ? static_cast<unsigned short>(impl_->port) : 0;
}

std::string ServerHandle::url() const {
    if (!impl_) {
        return {};
    }
    // 0.0.0.0 is a bind address, not somewhere to point a browser.
    const std::string host = impl_->host == "0.0.0.0" ? "127.0.0.1" : impl_->host;
    return std::format("http://{}:{}", host, impl_->port);
}

void ServerHandle::stop() noexcept {
    if (impl_) {
        impl_->shutdown();
    }
}

ServerHandle serve(const ServeOptions &options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(options.root, ec)) {
        throw Error{codes::kFatalWebServeBind, "cannot serve a directory that is not there",
                    options.root.string()};
    }

    auto impl = std::make_unique<ServerHandle::Impl>();
    impl->root = std::filesystem::weakly_canonical(options.root, ec);
    impl->host = options.host;

    // no-store on everything, for the reason scripts/serve_nocache.py documents:
    // the compute worker fetches its .wasm from inside a Web Worker, and browsers
    // do not apply a hard reload's cache bypass to worker-initiated requests -- so
    // a rebuilt worker module is otherwise served stale against a freshly loaded
    // main module, silently.
    impl->server.set_default_headers({{"Cache-Control", "no-store"}});
    teachMimeTypes(impl->server);

    // One mount point is the whole router. `/` resolves to index.html by
    // httplib's own convention, which is why the shell is named that rather than
    // viewer.html -- it means every static host, this one included, needs no
    // special case.
    if (!impl->server.set_mount_point("/", impl->root.string())) {
        throw Error{codes::kFatalWebServeBind, "cannot mount the staged root", impl->root.string()};
    }

    // Bind on *this* thread, so the port is known before `serve` returns and no
    // caller ever races the listener for it. Only the accept loop moves. Two
    // calls rather than one because `bind_to_any_port` takes no port at all --
    // asking for a specific one is a different function, not an argument.
    if (options.port == 0) {
        impl->port = impl->server.bind_to_any_port(options.host);
        if (impl->port < 0) {
            throw Error{codes::kFatalWebServeBind,
                        std::format("cannot bind {} to any port", options.host)};
        }
    } else {
        if (!impl->server.bind_to_port(options.host, options.port)) {
            throw Error{codes::kFatalWebServeBind,
                        std::format("cannot bind {}:{} -- in use?", options.host, options.port)};
        }
        impl->port = options.port;
    }

    ServerHandle::Impl *raw = impl.get();
    impl->thread = std::thread([raw] { raw->server.listen_after_bind(); });
    impl->server.wait_until_ready();

    return ServerHandle{std::move(impl)};
}

} // namespace nodehammer::web

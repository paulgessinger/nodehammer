#include "cli_common.hpp"

#include "web/open_browser.hpp"
#include "web/runtime_locator.hpp"
#include "web/stage.hpp"
#include "web/static_server.hpp"

#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <thread>

#ifdef _WIN32
#include <random>
#else
#include <cstdlib>
#endif

namespace {

namespace web = nodehammer::web;

/// A staging directory that cleans up after the server stops.
///
/// Under the system temp dir rather than beside the input: the input may be
/// read-only, may be a fixture directory under version control, and in the
/// no-argument case does not exist at all.
///
/// **Created by mkdtemp, not by name.** On Linux `temp_directory_path()` is
/// `/tmp`, shared by every local user, and what is staged here is the user's
/// project -- geometry that is often unpublished. A name we compose ourselves
/// is both predictable (so another user can pre-create it, or plant a symlink
/// for the copy to follow) and 0755 by default (so they can simply read it).
/// `mkdtemp` fixes all of that in one call: it picks the name, creates the
/// directory atomically, and creates it 0700.
///
/// Windows has no mkdtemp and does not need one -- its temp directory is already
/// per-user under %LOCALAPPDATA% -- so there a random name and an exclusive
/// create carry the uniqueness half alone.
class ScopedStagingDir {
  public:
    ScopedStagingDir() {
#ifdef _WIN32
        std::random_device rd;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                                   std::format("nodehammer-web-{:08x}{:08x}", rd(), rd());
            std::error_code ec;
            // create_directory reports false without error when it already
            // exists, which is exactly the collision we are retrying past.
            if (std::filesystem::create_directory(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        throw nodehammer::Error{nodehammer::codes::kFatalWebStage,
                                "cannot create a staging directory"};
#else
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "nodehammer-web-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw nodehammer::Error{nodehammer::codes::kFatalWebStage,
                                    "cannot create a staging directory", tmpl};
        }
        path_ = tmpl;
#endif
    }
    ~ScopedStagingDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScopedStagingDir(const ScopedStagingDir &) = delete;
    ScopedStagingDir &operator=(const ScopedStagingDir &) = delete;

    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/// The one piece of state that cannot be anything else.
///
/// `std::signal` takes a `void(int)` with no user parameter, so a handler cannot
/// be given anything to write to; it has to reach an object of static storage
/// duration. There is no design that removes that, only designs that hide it.
///
/// **A lock-free atomic, not `volatile sig_atomic_t`.** [support.signal] blesses
/// either one inside a handler, and the C idiom is the one that comes to mind
/// first — but this program is multithreaded (the server runs its own thread),
/// and `volatile` carries no cross-thread guarantees at all: it forbids eliding
/// and reordering the access, and says nothing about atomicity or about
/// happens-before. `std::atomic<bool>` is well-defined for both readers, and the
/// assertion below is what makes it legal in the handler rather than assumed to
/// be.
///
/// **`constinit`, and at namespace scope.** A function-local static would scope
/// the name better, and is what one reaches for to avoid a global — but it
/// carries a guard variable, and a guard is a lock. Touching one from a signal
/// handler is not async-signal-safe, and reasoning that "the guard is already
/// satisfied because something else ran first" makes correctness depend on call
/// order. `constinit` states at the declaration that there is no dynamic
/// initialization to guard, which is the property actually needed, checked by
/// the compiler rather than by argument.
static_assert(std::atomic<bool>::is_always_lock_free,
              "a signal handler may only touch a lock-free atomic ([support.signal])");
constinit std::atomic<bool> g_interrupted{false};

extern "C" void onInterrupt(int) { g_interrupted.store(true, std::memory_order_relaxed); }

/// Ctrl-C, borrowed for the duration of the wait and handed back.
///
/// This command is reachable as `nh.cli.run(["viewer", "--web"])`, where
/// permanently replacing Python's SIGINT handler would break every later
/// KeyboardInterrupt in the session. So the handler is scoped even though the
/// flag it writes cannot be.
class ScopedInterruptHandler {
  public:
    ScopedInterruptHandler() : previous_(std::signal(SIGINT, onInterrupt)) {
        g_interrupted.store(false, std::memory_order_relaxed);
    }
    ~ScopedInterruptHandler() {
        if (previous_ != SIG_ERR) {
            std::signal(SIGINT, previous_);
        }
        g_interrupted.store(false, std::memory_order_relaxed);
    }
    ScopedInterruptHandler(const ScopedInterruptHandler &) = delete;
    ScopedInterruptHandler &operator=(const ScopedInterruptHandler &) = delete;

    [[nodiscard]] static bool interrupted() {
        return g_interrupted.load(std::memory_order_relaxed);
    }

  private:
    void (*previous_)(int);
};

/// Read a parsed option, or its default.
///
/// The parser holds the values, so neither half of this command needs a shared
/// struct to pass them in — which is what lets the two halves live in different
/// binaries without a static tying them together.
std::string optionText(const CLI::App &sub, const char *name, std::string fallback = {}) {
    const CLI::Option *opt = sub.get_option(name);
    return opt->count() > 0 ? opt->as<std::string>() : std::move(fallback);
}

/// Native-only options *reject* under --web rather than being ignored.
/// `--screenshot` in particular would otherwise look like it had worked.
void refuseNativeOnlyOptions(const CLI::App &sub) {
    static constexpr std::string_view kWebSafe[] = {
        "--web",   "--port", "--host", "--no-browser", "--web-assets",
        "--title", "--help", "path",   "--config",     "--input"};
    for (const CLI::Option *opt : sub.get_options()) {
        if (opt->count() == 0 || opt->get_name().empty()) {
            continue;
        }
        const bool shared = std::ranges::any_of(kWebSafe, [opt](std::string_view name) {
            return opt->check_name(std::string{name}) || opt->get_name() == name;
        });
        if (!shared) {
            throw CLI::ValidationError(opt->get_name(),
                                       "is a native-viewer option and has no effect with --web");
        }
    }
}

} // namespace

namespace nodehammer::cli::detail {

void addViewerCommonOptions(CLI::App &sub) {
    // Unbound on purpose: the values live in the parser, so the native half can
    // read the same options without this half handing it anything.
    sub.add_option("path", "Project to open: a .nhproj archive or a directory")->type_name("PATH");
    sub.add_option("-c,--config", "TOML config file");
    sub.add_option("-i,--input", "Input geometry file (.nhb / .nhb.zst)");
    sub.add_option("--title", "Window or browser-tab title");
}

void addViewerWebOptions(CLI::App &sub) {
    auto *webOpt = sub.add_flag("--web", "Serve the wasm viewer and open it in a browser");
    // 0, not 8000: an ephemeral port cannot collide with `just wasm-serve` or
    // with a second copy of this command, and neither collision is interesting
    // enough to make the default worse.
    sub.add_option("--port", "Port to serve on (0 picks a free one)")->needs(webOpt);
    sub.add_option("--host", "Address to bind (default 127.0.0.1)")->needs(webOpt);
    sub.add_flag("--no-browser", "Print the URL, open nothing")->needs(webOpt);
    sub.add_option("--web-assets", "Directory holding the built wasm runtime")->needs(webOpt);
}

bool viewerWebRequested(const CLI::App &sub) { return sub.count("--web") > 0; }

void runViewerWeb(CLI::App &sub, const RunOptions &options) {
    refuseNativeOnlyOptions(sub);

    runOrReport("viewer", [&] {
        web::LadderInputs inputs{};
        inputs.explicitDir = optionText(sub, "--web-assets");
        inputs.embedderDir = options.webAssets;

        // Walked a second time only on the failure path, which is the one where
        // being helpful is worth a directory stat. See web/runtime_locator.hpp
        // for why the detail is not carried by the exception.
        const auto locate = [&] {
            try {
                return web::locateRuntime(inputs);
            } catch (const nodehammer::Error &) {
                std::print(stderr, "{}\n", web::explainLadder(web::walkLadder(inputs)));
                throw;
            }
        };
        const web::RuntimeLocation runtime = locate();

        const ScopedStagingDir staging;
        web::StageOptions stage{};
        stage.runtime = runtime.dir;
        stage.target = staging.path();
        stage.title = optionText(sub, "--title");
        stage.project = optionText(sub, "path");
        stage.config = optionText(sub, "--config");
        stage.geometry = optionText(sub, "--input");
        const web::StagedRoot staged = web::stageRoot(stage);

        const std::string host = optionText(sub, "--host", "127.0.0.1");
        web::ServeOptions serve{};
        serve.root = staged.dir;
        serve.host = host;
        serve.port = static_cast<unsigned short>(
            sub.count("--port") > 0 ? sub.get_option("--port")->as<int>() : 0);
        web::ServerHandle server = web::serve(serve);

        if (host != "127.0.0.1" && host != "localhost") {
            std::println(stderr,
                         "warning: bound to {} — anything that can reach this machine can reach "
                         "the viewer. Loopback plus SSH forwarding is the safer answer.",
                         host);
        }

        std::println("nodehammer viewer — {} mode, runtime {}",
                     staged.posture == web::Posture::Viewer ? "viewer" : "application",
                     runtime.version);
        std::println("serving {}", server.url());

        if (sub.count("--no-browser") == 0 && !web::openInBrowser(server.url())) {
            std::println(stderr, "could not open a browser; the URL above still works");
        }
        std::println("Ctrl-C to stop.");
        std::fflush(stdout);

        // The wait loop lives here, in the front door, and not in `serve`: an
        // embedder that blocked would have no way to be interrupted, and a
        // notebook that blocked would be over. See web/static_server.hpp.
        //
        // Interruption *returns* rather than terminating, so the staging
        // directory above is actually removed — a handler-less Ctrl-C runs no
        // destructors and leaves 7 MB behind on every run.
        const ScopedInterruptHandler interruptible;
        while (server.running() && !ScopedInterruptHandler::interrupted()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (ScopedInterruptHandler::interrupted()) {
            std::println("");
            std::println("stopping.");
        }
        server.stop();
    });
}

void registerCmdViewer(CLI::App &app, const RunOptions &options) {
    auto *sub = app.add_subcommand("viewer", "Open the interactive 3D viewer");
    addViewerCommonOptions(*sub);
    addViewerWebOptions(*sub);

    // Replaced wholesale by the native half where there is a window, which is
    // what keeps the dispatch in one place per binary instead of a callback slot
    // one half fills in for the other.
    //
    // `options` is the caller's object and outlives the parse (see the
    // `Registrar` comment in run_internal.hpp), so capturing the pointer keeps
    // this a reference to what the front door actually said rather than a copy
    // taken at registration time.
    sub->callback([sub, &options] {
        if (!viewerWebRequested(*sub)) {
            // The build has no window. Name the flag that does work rather than
            // failing as though the command were unknown — this is the normal
            // state inside a wheel, and on a headless machine it is the state
            // the user is trying to get around.
            throw nodehammer::Error{nodehammer::codes::kFatalCliUsage,
                                    "this build has no native viewer; use `nodehammer viewer "
                                    "--web` to open the viewer in a browser"};
        }
        runViewerWeb(*sub, options);
    });
}

} // namespace nodehammer::cli::detail

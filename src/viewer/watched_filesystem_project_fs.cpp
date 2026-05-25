#include <nodehammer/viewer/watched_filesystem_project_fs.hpp>

#include <wtr/watcher.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace nodehammer::viewer {

namespace {

/// True when any segment of `rel` (a path relative to the mount root)
/// starts with `.`, excluding the `.`/`..` navigation segments. Mirrors
/// `FilesystemProjectFs`'s default `skip_hidden_files` filter so we don't
/// rebuild on noise the inner walk would skip anyway (.DS_Store, .git,
/// editor swap files). Assumes the inner FS uses the default
/// `skip_hidden_files == true`.
bool hasHiddenSegment(const std::filesystem::path &rel) {
    for (const auto &seg : rel) {
        const auto s = seg.string();
        if (!s.empty() && s.front() == '.' && s != "." && s != "..") {
            return true;
        }
    }
    return false;
}

/// wtr emits status/error events with `path_type::watcher`; the first
/// callback is always one of these ("s/self/live@…" on success,
/// "e/self/live@…" on failure). Errors are flagged by an "e/" prefix.
bool isWatcherStatus(const wtr::event &ev) {
    return ev.path_type == wtr::event::path_type::watcher;
}
bool isWatcherError(const wtr::event &ev) {
    const auto s = ev.path_name.string();
    return s.size() >= 2 && s[0] == 'e' && s[1] == '/';
}

} // namespace

struct WatchedFilesystemProjectFs::Impl {
    /// Change state shared with the watch callback through a `shared_ptr`.
    /// wtr delivers callbacks on a background thread and may fire one
    /// late (during/after teardown of the FSEvents stream), so the
    /// callback must never reach back into the decorator's `this`/`impl_`.
    /// Co-owning this block keeps the mutex + flags alive for any
    /// in-flight callback even if the decorator is already gone.
    struct Shared {
        std::mutex mu;
        bool dirty{false};
        std::chrono::steady_clock::time_point last_event{};
        std::optional<std::string> watch_error;
    };

    std::unique_ptr<FilesystemProjectFs> inner;
    std::chrono::milliseconds debounce;
    std::shared_ptr<Shared> shared;

    // Declared last so its destructor (which blocks until the background
    // thread stops) runs before `inner`/`shared` are torn down.
    std::optional<wtr::watch> watch;

    Impl(std::unique_ptr<FilesystemProjectFs> in, std::chrono::milliseconds db)
        : inner(std::move(in)), debounce(db), shared(std::make_shared<Shared>()) {}
};

WatchedFilesystemProjectFs::WatchedFilesystemProjectFs(std::unique_ptr<FilesystemProjectFs> inner,
                                                       std::chrono::milliseconds debounce)
    : impl_(std::make_unique<Impl>(std::move(inner), debounce)) {
    // Begin watching the mount root. The callback runs on wtr's background
    // thread: it filters watcher-status and hidden-path noise, then marks
    // the shared state dirty. It captures only `shared` (a shared_ptr) and
    // `root` by value — never `this` — so a late callback is always safe.
    // The LogSink is untouched here (main-thread only); watcher errors are
    // stashed and surfaced from poll().
    const auto root = impl_->inner->root();
    auto shared = impl_->shared;
    impl_->watch.emplace(root, [shared, root](const wtr::event &ev) {
        if (isWatcherStatus(ev)) {
            if (isWatcherError(ev)) {
                std::lock_guard<std::mutex> lk(shared->mu);
                shared->watch_error = ev.path_name.string();
            }
            return;
        }
        const auto rel = ev.path_name.lexically_relative(root);
        if (!rel.empty() && hasHiddenSegment(rel)) {
            return;
        }
        std::lock_guard<std::mutex> lk(shared->mu);
        shared->dirty = true;
        shared->last_event = std::chrono::steady_clock::now();
    });
}

WatchedFilesystemProjectFs::~WatchedFilesystemProjectFs() = default;

void WatchedFilesystemProjectFs::notifyChanged() {
    auto &s = *impl_->shared;
    std::lock_guard<std::mutex> lk(s.mu);
    s.dirty = true;
    s.last_event = std::chrono::steady_clock::now();
}

void WatchedFilesystemProjectFs::poll() {
    impl_->inner->poll(); // forward (no-op today, future-proof)

    bool fire = false;
    std::optional<std::string> err;
    {
        auto &s = *impl_->shared;
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.watch_error) {
            err.swap(s.watch_error);
        }
        if (s.dirty && std::chrono::steady_clock::now() - s.last_event >= impl_->debounce) {
            s.dirty = false;
            fire = true;
        }
    }

    if (err) {
        pushWarning("filesystem watcher: " + *err);
    }
    if (fire) {
        impl_->inner->rescan(); // bumps generation() → BuildSession re-walks
    }
}

void WatchedFilesystemProjectFs::setLogSink(LogSink *sink) noexcept {
    // Wire our own diagnostics, then forward so the inner walk's warnings
    // reach the App's notification sink too.
    LogSinkHolder::setLogSink(sink);
    impl_->inner->setLogSink(sink);
}

ProjectFsStatus WatchedFilesystemProjectFs::status() const { return impl_->inner->status(); }

std::span<const ProjectProgress> WatchedFilesystemProjectFs::progress() const {
    return impl_->inner->progress();
}

std::string_view WatchedFilesystemProjectFs::name() const { return impl_->inner->name(); }

std::span<const std::string> WatchedFilesystemProjectFs::warnings() const {
    return impl_->inner->warnings();
}

ProjectDropDecision
WatchedFilesystemProjectFs::planAddPath(const std::filesystem::path &path) const {
    return impl_->inner->planAddPath(path);
}

ProjectDropDecision
WatchedFilesystemProjectFs::planAddBytes(std::string_view filename,
                                         std::span<const std::byte> bytes) const {
    return impl_->inner->planAddBytes(filename, bytes);
}

void WatchedFilesystemProjectFs::addPath(const std::filesystem::path &path) {
    impl_->inner->addPath(path);
}

void WatchedFilesystemProjectFs::addBytes(std::string_view filename,
                                          std::span<const std::byte> bytes) {
    impl_->inner->addBytes(filename, bytes);
}

ResolveResult WatchedFilesystemProjectFs::resolve(std::string_view key) const {
    return impl_->inner->resolve(key);
}

std::uint64_t WatchedFilesystemProjectFs::generation() const { return impl_->inner->generation(); }

std::span<const DirNode> WatchedFilesystemProjectFs::list(std::string_view dir) const {
    return impl_->inner->list(dir);
}

void WatchedFilesystemProjectFs::rescan() { impl_->inner->rescan(); }

} // namespace nodehammer::viewer

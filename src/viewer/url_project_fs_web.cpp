#include <nodehammer/viewer/url_project_fs.hpp>

#include <emscripten/fetch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nodehammer::viewer {

namespace {

// Force a leading-slash, lexically-normal MEMFS-style path. Stage 1 normalised
// these for MEMFS write paths; Stage 2 keeps the same canonical form for keys
// so that BuildSession's resolveIncludeKey arithmetic and direct caller-side
// keys collapse to the same string before fetch dedup runs.
std::string normalizeKey(std::string_view raw) {
    auto p = std::filesystem::path(raw).lexically_normal();
    auto s = p.generic_string();
    if (s.empty() || s.front() != '/') {
        s = "/" + s;
    }
    return s;
}

} // namespace

struct UrlProjectFs::Impl {
    enum class State { InFlight, Ready, Failed };

    struct Entry {
        State state{State::InFlight};
        ByteBuffer bytes; // populated when Ready (refcount handle)
        std::uint64_t bytes_done{0};
        std::uint64_t bytes_total{0};
        bool failed_visible{false}; // mirrors `failed` in ProjectProgress
    };

    std::string asset_base;
    /// Sticky-once-set: any fetch failure flips this true. Per-fetch
    /// messages are pushed to the LogSink at the moment they occur; we
    /// no longer retain text here.
    bool errored{false};
    bool any_in_flight{false};

    /// Backpointer to the outer `ProjectFs` so the static fetch callbacks
    /// can route diagnostics through `pushError`/`pushWarning`. Set in
    /// `UrlProjectFs::setLogSink` (which always runs once at App-wire
    /// time, well before any fetch can complete).
    UrlProjectFs *parent{nullptr};

    /// Stable storage so emscripten_fetch callbacks can hold raw pointers
    /// across the suspend without worrying about hash-map rehash.
    std::unordered_map<std::string, std::unique_ptr<Entry>> entries;

    /// Display order (UI shows fetches in the order resolve was first
    /// called). Strings are the same canonical keys used in `entries`.
    std::vector<std::string> order;

    /// Snapshot of `entries` used by `progress()` so the returned span
    /// has a stable address. Recomputed lazily; cheap given the small
    /// file count.
    mutable std::vector<ProjectProgress> progress_view;
    mutable std::uint64_t progress_view_gen{0};

    /// Per-directory listing cache for `list()`. URL keys can include
    /// `/`-separated path components (e.g. `subdir/common.toml`); we
    /// synthesise children of any prefix on demand. Cleared en masse
    /// when `dir_cache_gen` falls behind `generation`.
    mutable std::unordered_map<std::string, std::vector<DirNode>> dir_cache;
    mutable std::uint64_t dir_cache_gen{static_cast<std::uint64_t>(-1)};

    std::uint64_t generation{0};

    struct FetchCtx {
        Impl *self;
        std::string key;
    };

    static void onSuccess(emscripten_fetch_t *fetch);
    static void onError(emscripten_fetch_t *fetch);
    static void onProgress(emscripten_fetch_t *fetch);

    /// Returns a pointer to the Entry for `key`, creating one + kicking
    /// off the fetch if it doesn't already exist. The pointer is stable
    /// for the lifetime of this Impl.
    Entry *startFetch(const std::string &key);

    void recountInFlight() {
        any_in_flight = false;
        for (const auto &[k, e] : entries) {
            (void)k;
            if (e->state == State::InFlight) {
                any_in_flight = true;
                return;
            }
        }
    }
};

UrlProjectFs::Impl::Entry *UrlProjectFs::Impl::startFetch(const std::string &key) {
    if (auto it = entries.find(key); it != entries.end()) {
        return it->second.get();
    }

    auto e = std::make_unique<Entry>();
    auto *raw = e.get();
    entries.emplace(key, std::move(e));
    order.push_back(key);
    any_in_flight = true;
    ++generation;

    const std::string fetch_url = asset_base + key;
    std::fprintf(stderr, "url_project_fs: enqueue %s (fetch %s)\n", key.c_str(), fetch_url.c_str());

    auto *ctx = new FetchCtx{this, key};

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = &Impl::onSuccess;
    attr.onerror = &Impl::onError;
    attr.onprogress = &Impl::onProgress;
    attr.userData = ctx;
    emscripten_fetch(&attr, fetch_url.c_str());

    return raw;
}

void UrlProjectFs::Impl::onProgress(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    if (ctx == nullptr) {
        return;
    }
    auto it = ctx->self->entries.find(ctx->key);
    if (it == ctx->self->entries.end()) {
        return;
    }
    it->second->bytes_done = static_cast<std::uint64_t>(fetch->dataOffset + fetch->numBytes);
    it->second->bytes_total = static_cast<std::uint64_t>(fetch->totalBytes);
}

void UrlProjectFs::Impl::onSuccess(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    Impl *self = ctx->self;
    auto it = self->entries.find(ctx->key);

    const auto size = static_cast<std::size_t>(fetch->numBytes);
    if (it != self->entries.end()) {
        auto &e = *it->second;
        std::vector<std::byte> buf(reinterpret_cast<const std::byte *>(fetch->data),
                                   reinterpret_cast<const std::byte *>(fetch->data) + size);
        e.bytes = ByteBuffer{std::move(buf)};
        e.bytes_done = static_cast<std::uint64_t>(size);
        if (e.bytes_total == 0) {
            e.bytes_total = e.bytes_done;
        }
        e.state = State::Ready;
    }

    emscripten_fetch_close(fetch);
    delete ctx;

    self->recountInFlight();
    ++self->generation;
}

void UrlProjectFs::Impl::onError(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    Impl *self = ctx->self;
    auto it = self->entries.find(ctx->key);

    if (it != self->entries.end()) {
        auto &e = *it->second;
        e.state = State::Failed;
        e.failed_visible = true;
    }
    if (self->parent != nullptr) {
        self->parent->pushError("fetch failed (" + std::to_string(fetch->status) +
                                "): " + ctx->key);
    }
    self->errored = true;

    emscripten_fetch_close(fetch);
    delete ctx;

    self->recountInFlight();
    ++self->generation;
}

UrlProjectFs::UrlProjectFs() : impl_(std::make_unique<Impl>()) {}
UrlProjectFs::~UrlProjectFs() = default;

void UrlProjectFs::setAssetBase(std::string asset_base) {
    while (!asset_base.empty() && asset_base.back() == '/') {
        asset_base.pop_back();
    }
    impl_->asset_base = std::move(asset_base);
}

void UrlProjectFs::poll() {}

void UrlProjectFs::setLogSink(LogSink *sink) noexcept {
    // Base flushes any constructor-time pending diagnostics here.
    ProjectFs::setLogSink(sink);
    // The parent backpointer doesn't depend on the sink itself; we set it
    // here because this method is the App's "the FS is now wired" hook.
    // Static fetch callbacks route through `parent->pushError`, which the
    // base forwards to the live sink (or buffers if `sink == nullptr`).
    impl_->parent = this;
}

ProjectFsStatus UrlProjectFs::status() const {
    if (impl_->errored) {
        return ProjectFsStatus::Error;
    }
    if (impl_->any_in_flight) {
        return ProjectFsStatus::Fetching;
    }
    return ProjectFsStatus::Idle;
}

std::span<const ProjectProgress> UrlProjectFs::progress() const {
    if (impl_->progress_view_gen != impl_->generation) {
        impl_->progress_view.clear();
        impl_->progress_view.reserve(impl_->order.size());
        for (const auto &k : impl_->order) {
            const auto &e = *impl_->entries.at(k);
            ProjectProgress p;
            p.url = k;
            p.bytes_done = e.bytes_done;
            p.bytes_total = e.bytes_total;
            p.done = e.state == Impl::State::Ready;
            p.failed = e.failed_visible;
            impl_->progress_view.push_back(std::move(p));
        }
        impl_->progress_view_gen = impl_->generation;
    }
    return {impl_->progress_view.data(), impl_->progress_view.size()};
}

ProjectDropDecision UrlProjectFs::planAddPath(const std::filesystem::path &path) const {
    return ProjectDropDecision{
        ProjectDropDecision::Kind::Reject,
        "Cannot add file to URL project",
        "This project is loaded from URLs and is read-only.\n\nDropped file \"" +
            path.filename().string() + "\" was not added.",
        "OK",
        {},
    };
}

ProjectDropDecision UrlProjectFs::planAddBytes(std::string_view filename,
                                               std::span<const std::byte> /*bytes*/) const {
    return ProjectDropDecision{
        ProjectDropDecision::Kind::Reject,
        "Cannot add file to URL project",
        "This project is loaded from URLs and is read-only.\n\nUploaded file \"" +
            std::string{filename} + "\" was not added.",
        "OK",
        {},
    };
}

ResolveResult UrlProjectFs::resolve(std::string_view key) const {
    const auto canonical = normalizeKey(key);
    auto *entry = impl_->startFetch(canonical);

    switch (entry->state) {
    case Impl::State::Ready:
        return ResolveResult{ResolveStatus::Ready, OpenedFile{canonical, entry->bytes}, {}, {}};
    case Impl::State::InFlight:
        return ResolveResult{ResolveStatus::Pending, {}, {}, {}};
    case Impl::State::Failed:
        return ResolveResult{ResolveStatus::Error, {}, {}, "fetch failed: " + canonical};
    }
    return ResolveResult{ResolveStatus::Error, {}, {}, "unknown fetch state"};
}

std::uint64_t UrlProjectFs::generation() const { return impl_->generation; }

namespace {

/// Strip a single leading `/` from a normalised key so subsequent path
/// arithmetic produces clean component splits. `normalizeKey` always
/// prepends `/`, but for `list()` we want to treat the storage as
/// rooted at the bag root, not at filesystem-root.
std::string_view stripLeadingSlash(std::string_view k) {
    if (!k.empty() && k.front() == '/') {
        k.remove_prefix(1);
    }
    return k;
}

std::string baseKey(std::string_view key) {
    auto pos = key.find_last_of('/');
    if (pos == std::string_view::npos) {
        return std::string{key};
    }
    return std::string{key.substr(pos + 1)};
}

/// Returns true when `child` is an immediate descendant of `parent` —
/// i.e. `child` has the form `parent/<segment>` with no further `/`.
/// Empty parent matches top-level keys with no `/`.
bool isImmediateChildOf(std::string_view child, std::string_view parent) {
    if (parent.empty()) {
        return child.find('/') == std::string_view::npos;
    }
    if (child.size() <= parent.size() + 1) {
        return false;
    }
    if (child.substr(0, parent.size()) != parent) {
        return false;
    }
    if (child[parent.size()] != '/') {
        return false;
    }
    return child.find('/', parent.size() + 1) == std::string_view::npos;
}

} // namespace

std::span<const DirNode> UrlProjectFs::list(std::string_view dir) const {
    if (impl_->dir_cache_gen != impl_->generation) {
        impl_->dir_cache.clear();
        impl_->dir_cache_gen = impl_->generation;
    }

    const std::string norm{stripLeadingSlash(dir == "/" ? std::string_view{} : dir)};

    if (auto it = impl_->dir_cache.find(norm); it != impl_->dir_cache.end()) {
        return {it->second.data(), it->second.size()};
    }

    // Validate non-root: it must be a real prefix of at least one fetched
    // key (i.e. an implicit directory we'd synthesise). Otherwise return
    // an empty span without caching.
    if (!norm.empty()) {
        bool exists = false;
        for (const auto &raw : impl_->order) {
            std::string_view k = stripLeadingSlash(raw);
            if (k.size() > norm.size() && k.substr(0, norm.size()) == norm &&
                k[norm.size()] == '/') {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return {};
        }
    }

    // Synthesise immediate children for `norm`: leaf entries whose
    // parent is `norm`, plus implicit directories formed by the next
    // path segment of any deeper entry.
    std::vector<DirNode> children;
    std::set<std::string> dir_segments;

    for (const auto &raw : impl_->order) {
        std::string_view k = stripLeadingSlash(raw);
        if (!norm.empty()) {
            if (k.size() <= norm.size() + 1 || k.substr(0, norm.size()) != norm ||
                k[norm.size()] != '/') {
                continue;
            }
        }
        if (isImmediateChildOf(k, norm)) {
            const auto &e = *impl_->entries.at(raw);
            DirNode node;
            node.name = baseKey(k);
            node.key = std::string{k};
            node.is_directory = false;
            node.bytes = e.bytes_total;
            children.push_back(std::move(node));
            continue;
        }
        // Deeper entry: its first segment under `norm` is an implicit dir.
        const auto rel_start = norm.empty() ? 0 : norm.size() + 1;
        const auto next_slash = k.find('/', rel_start);
        if (next_slash != std::string_view::npos) {
            dir_segments.insert(std::string{k.substr(0, next_slash)});
        }
    }

    for (const auto &dk : dir_segments) {
        DirNode node;
        node.name = baseKey(dk);
        node.key = dk;
        node.is_directory = true;
        node.bytes = 0;
        children.push_back(std::move(node));
    }

    std::sort(children.begin(), children.end(),
              [](const DirNode &a, const DirNode &b) { return a.name < b.name; });

    auto [ins, _] = impl_->dir_cache.emplace(norm, std::move(children));
    return {ins->second.data(), ins->second.size()};
}

} // namespace nodehammer::viewer

#include <nodehammer/viewer/url_project_fs.hpp>

#include <emscripten/fetch.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
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
        std::vector<std::byte> bytes; // valid when Ready
        std::string error;            // when Failed
        std::uint64_t bytes_done{0};
        std::uint64_t bytes_total{0};
        bool failed_visible{false}; // mirrors `failed` in ProjectProgress
    };

    std::string asset_base;
    std::string error; // first hard failure across all fetches
    bool any_in_flight{false};

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
        e.bytes.assign(reinterpret_cast<const std::byte *>(fetch->data),
                       reinterpret_cast<const std::byte *>(fetch->data) + size);
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
        e.error = "fetch failed (" + std::to_string(fetch->status) + "): " + ctx->key;
    }
    if (self->error.empty()) {
        self->error = "fetch failed (" + std::to_string(fetch->status) + "): " + ctx->key;
    }

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

ProjectFsStatus UrlProjectFs::status() const {
    if (!impl_->error.empty()) {
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

const std::string &UrlProjectFs::errorMessage() const { return impl_->error; }

ResolveResult UrlProjectFs::resolve(std::string_view key) const {
    const auto canonical = normalizeKey(key);
    auto *entry = impl_->startFetch(canonical);

    switch (entry->state) {
    case Impl::State::Ready:
        return ResolveResult{ResolveStatus::Ready,
                             OpenedFile{canonical, std::span<const std::byte>{entry->bytes}},
                             {},
                             {}};
    case Impl::State::InFlight:
        return ResolveResult{ResolveStatus::Pending, {}, {}, {}};
    case Impl::State::Failed:
        return ResolveResult{ResolveStatus::Error, {}, {}, entry->error};
    }
    return ResolveResult{ResolveStatus::Error, {}, {}, "unknown fetch state"};
}

std::uint64_t UrlProjectFs::generation() const { return impl_->generation; }

} // namespace nodehammer::viewer

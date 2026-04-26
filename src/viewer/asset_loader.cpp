#include <nodehammer/viewer/asset_loader.hpp>

#include <nodehammer/config/config_loader.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#include <sys/stat.h>
#endif

namespace nodehammer::viewer {

namespace {

#ifdef __EMSCRIPTEN__

// Walk the parent dirs of `path` (a MEMFS-rooted path like `/odd/base.toml`)
// and `mkdir` each one. EEXIST is the success case for re-runs.
void make_parent_dirs(const std::string &path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (!current.empty()) {
                ::mkdir(current.c_str(), 0755);
            }
            current.push_back('/');
        } else {
            current.push_back(path[i]);
        }
    }
    // The final segment is the file name itself — don't mkdir it.
}

bool write_file(const std::string &path, const char *data, size_t size) {
    make_parent_dirs(path);
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

// Resolve an `include` entry (relative to the config file) to an absolute
// MEMFS / URL path. Mirrors the std::filesystem::canonical step the real
// loader does at config_loader.cpp:86, but lexically only — MEMFS doesn't
// have a working canonical().
std::string resolve_include_url(const std::string &config_url, const std::string &rel) {
    std::filesystem::path base = std::filesystem::path(config_url).parent_path();
    std::filesystem::path joined = (base / rel).lexically_normal();
    return joined.generic_string();
}

#endif // __EMSCRIPTEN__

} // namespace

struct AssetLoader::Impl {
    LoadState state{LoadState::Idle};
    std::string error;
    std::string config_url;
    std::string input_url;
    std::vector<AssetProgress> entries;
    std::unordered_set<std::string> seen;

#ifdef __EMSCRIPTEN__
    // Per-fetch context: small enough to heap-allocate one per request.
    struct FetchCtx {
        Impl *self;
        size_t index;
    };

    enum class Kind { Config, Include, Input };

    void enqueue(const std::string &url, Kind kind);
    static void on_success(emscripten_fetch_t *fetch);
    static void on_error(emscripten_fetch_t *fetch);
    static void on_progress(emscripten_fetch_t *fetch);
    void after_file_landed(size_t index, Kind kind);
    void check_done();

    // We don't track Kind on the entry itself (the user-facing AssetProgress
    // shouldn't grow that field), so map index → kind here.
    std::vector<Kind> kinds;
#endif
};

#ifdef __EMSCRIPTEN__

void AssetLoader::Impl::enqueue(const std::string &url, Kind kind) {
    if (!seen.insert(url).second) {
        return;
    }
    const size_t index = entries.size();
    entries.push_back(AssetProgress{url, 0, 0, false, false});
    kinds.push_back(kind);

    auto *ctx = new FetchCtx{this, index};

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = &Impl::on_success;
    attr.onerror = &Impl::on_error;
    attr.onprogress = &Impl::on_progress;
    attr.userData = ctx;
    emscripten_fetch(&attr, url.c_str());
}

void AssetLoader::Impl::on_progress(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    if (ctx == nullptr) {
        return;
    }
    auto &entry = ctx->self->entries[ctx->index];
    entry.bytes_done = static_cast<std::uint64_t>(fetch->dataOffset + fetch->numBytes);
    entry.bytes_total = static_cast<std::uint64_t>(fetch->totalBytes);
}

void AssetLoader::Impl::on_success(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    Impl *self = ctx->self;
    const size_t index = ctx->index;
    const Kind kind = self->kinds[index];

    auto &entry = self->entries[index];
    const bool wrote = write_file(entry.url, fetch->data, static_cast<size_t>(fetch->numBytes));
    entry.bytes_done = static_cast<std::uint64_t>(fetch->numBytes);
    if (entry.bytes_total == 0) {
        entry.bytes_total = entry.bytes_done;
    }
    entry.done = true;
    entry.failed = !wrote;

    emscripten_fetch_close(fetch);
    delete ctx;

    if (!wrote) {
        self->state = LoadState::Error;
        self->error = "failed to write " + entry.url + " to MEMFS";
        return;
    }

    self->after_file_landed(index, kind);
    self->check_done();
}

void AssetLoader::Impl::on_error(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    Impl *self = ctx->self;
    const size_t index = ctx->index;

    auto &entry = self->entries[index];
    entry.done = true;
    entry.failed = true;

    self->state = LoadState::Error;
    self->error = "fetch failed (" + std::to_string(fetch->status) + "): " + entry.url;

    emscripten_fetch_close(fetch);
    delete ctx;
}

void AssetLoader::Impl::after_file_landed(size_t index, Kind kind) {
    if (kind == Kind::Input) {
        return;
    }
    // Both Config and Include kinds may declare further includes.
    const auto &url = entries[index].url;
    auto includes = ConfigLoader::peekIncludes(url);
    for (const auto &rel : includes) {
        enqueue(resolve_include_url(url, rel), Kind::Include);
    }
}

void AssetLoader::Impl::check_done() {
    if (state == LoadState::Error) {
        return;
    }
    for (const auto &e : entries) {
        if (!e.done) {
            return;
        }
        if (e.failed) {
            return;
        }
    }
    state = LoadState::Ready;
}

#endif // __EMSCRIPTEN__

AssetLoader::AssetLoader() : impl_(std::make_unique<Impl>()) {}
AssetLoader::~AssetLoader() = default;

void AssetLoader::start(std::string config_url, std::string input_url) {
    impl_->config_url = std::move(config_url);
    impl_->input_url = std::move(input_url);

#ifdef __EMSCRIPTEN__
    impl_->state = LoadState::Fetching;
    impl_->enqueue(impl_->config_url, Impl::Kind::Config);
    impl_->enqueue(impl_->input_url, Impl::Kind::Input);
#else
    // Native: files already on disk; nothing to fetch. The async path is
    // emscripten-only by design (see plan).
    impl_->state = LoadState::Ready;
#endif
}

void AssetLoader::poll() {
    // No-op: web is callback-driven, native resolves synchronously in start().
}

LoadState AssetLoader::state() const { return impl_->state; }

std::span<const AssetProgress> AssetLoader::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

const std::string &AssetLoader::error_message() const { return impl_->error; }
const std::string &AssetLoader::config_path() const { return impl_->config_url; }
const std::string &AssetLoader::input_path() const { return impl_->input_url; }

} // namespace nodehammer::viewer

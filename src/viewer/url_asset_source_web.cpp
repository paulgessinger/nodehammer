#include <nodehammer/viewer/url_asset_source.hpp>

#include <nodehammer/config/config_loader.hpp>

#include <emscripten/fetch.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace nodehammer::viewer {

namespace {

// Walk the parent dirs of `path` (a MEMFS-rooted path like `/odd/base.toml`)
// and `mkdir` each one. EEXIST is the success case for re-runs; any other
// errno is logged so silent MEMFS failures stop hiding from us.
bool makeParentDirs(const std::string &path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (!current.empty()) {
                if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    std::fprintf(stderr, "url_asset_source: mkdir(%s) failed: %s (errno=%d)\n",
                                 current.c_str(), std::strerror(errno), errno);
                    return false;
                }
            }
            current.push_back('/');
        } else {
            current.push_back(path[i]);
        }
    }
    return true;
}

bool writeFile(const std::string &path, const char *data, size_t size) {
    if (!makeParentDirs(path)) {
        std::fprintf(stderr,
                     "url_asset_source: writeFile(%s, size=%zu) aborted: "
                     "parent dirs missing\n",
                     path.c_str(), size);
        return false;
    }
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "url_asset_source: fopen(%s, wb) failed: %s (errno=%d)\n",
                     path.c_str(), std::strerror(errno), errno);
        return false;
    }
    const size_t written = std::fwrite(data, 1, size, f);
    if (written != size) {
        std::fprintf(stderr,
                     "url_asset_source: short write for %s: %zu of %zu bytes "
                     "(errno=%d: %s)\n",
                     path.c_str(), written, size, errno, std::strerror(errno));
    }
    if (std::fclose(f) != 0) {
        std::fprintf(stderr, "url_asset_source: fclose(%s) failed: %s (errno=%d)\n", path.c_str(),
                     std::strerror(errno), errno);
        return false;
    }
    if (written == size) {
        std::error_code ec;
        const bool exists_now = std::filesystem::exists(path, ec);
        const auto size_now = exists_now ? std::filesystem::file_size(path, ec) : 0;
        std::fprintf(stderr,
                     "url_asset_source: wrote %s (%zu bytes; post-write exists=%d size=%zu)\n",
                     path.c_str(), size, exists_now ? 1 : 0, static_cast<size_t>(size_now));
    }
    return written == size;
}

// Force a MEMFS-rooted absolute path. `path("foo").parent_path()` yields an
// empty path on relative inputs, which then makes `(empty / rel)` come out
// without the leading slash — different string, breaks the `seen` dedup,
// and `fopen` writes to the CWD instead of `/`. Normalising up-front keeps
// every URL canonical so the dedup set actually deduplicates.
std::string normalizeMemfsPath(std::string_view url) {
    auto p = std::filesystem::path(url).lexically_normal();
    auto s = p.generic_string();
    if (s.empty() || s.front() != '/') {
        s = "/" + s;
    }
    return s;
}

// Resolve an `include` entry (relative to the config file) to an absolute
// MEMFS / URL path. Mirrors the std::filesystem::canonical step the real
// loader does, but lexically only — at peek time the include file is not
// yet on MEMFS, so canonical() would throw.
std::string resolveIncludeUrl(const std::string &config_url, const std::string &rel) {
    std::filesystem::path base = std::filesystem::path(config_url).parent_path();
    std::filesystem::path joined = (base / rel).lexically_normal();
    return normalizeMemfsPath(joined.generic_string());
}

} // namespace

struct UrlAssetSource::Impl {
    LoadState state{LoadState::Idle};
    std::string error;
    std::filesystem::path config_path;
    std::filesystem::path input_path;
    std::string asset_base;
    std::vector<AssetProgress> entries;
    std::unordered_set<std::string> seen;

    struct FetchCtx {
        Impl *self;
        size_t index;
    };

    enum class Kind { Config, Include, Input };

    void enqueue(const std::string &url, Kind kind);
    static void onSuccess(emscripten_fetch_t *fetch);
    static void onError(emscripten_fetch_t *fetch);
    static void onProgress(emscripten_fetch_t *fetch);
    void afterFileLanded(size_t index, Kind kind);
    void checkDone();

    std::vector<Kind> kinds;
};

void UrlAssetSource::Impl::enqueue(const std::string &url, Kind kind) {
    if (!seen.insert(url).second) {
        std::fprintf(stderr, "url_asset_source: enqueue dedup'd %s\n", url.c_str());
        return;
    }
    // url is the MEMFS path (always rooted at `/`). The fetch URL is the
    // same path prefixed by asset_base for sub-path deployments; leaving
    // asset_base empty keeps the legacy server-root behaviour.
    const std::string fetch_url = asset_base + url;
    std::fprintf(stderr, "url_asset_source: enqueue %s (fetch %s)\n", url.c_str(),
                 fetch_url.c_str());
    const size_t index = entries.size();
    entries.push_back(AssetProgress{url, 0, 0, false, false});
    kinds.push_back(kind);

    auto *ctx = new FetchCtx{this, index};

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = &Impl::onSuccess;
    attr.onerror = &Impl::onError;
    attr.onprogress = &Impl::onProgress;
    attr.userData = ctx;
    emscripten_fetch(&attr, fetch_url.c_str());
}

void UrlAssetSource::Impl::onProgress(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    if (ctx == nullptr) {
        return;
    }
    auto &entry = ctx->self->entries[ctx->index];
    entry.bytes_done = static_cast<std::uint64_t>(fetch->dataOffset + fetch->numBytes);
    entry.bytes_total = static_cast<std::uint64_t>(fetch->totalBytes);
}

void UrlAssetSource::Impl::onSuccess(emscripten_fetch_t *fetch) {
    auto *ctx = static_cast<FetchCtx *>(fetch->userData);
    Impl *self = ctx->self;
    const size_t index = ctx->index;
    const Kind kind = self->kinds[index];

    auto &entry = self->entries[index];
    const bool wrote = writeFile(entry.url, fetch->data, static_cast<size_t>(fetch->numBytes));
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

    self->afterFileLanded(index, kind);
    self->checkDone();
}

void UrlAssetSource::Impl::onError(emscripten_fetch_t *fetch) {
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

void UrlAssetSource::Impl::afterFileLanded(size_t index, Kind kind) {
    if (kind == Kind::Input) {
        return;
    }
    // Copy the URL by value: enqueue() below push_backs into `entries`,
    // which can reallocate and invalidate any reference into the vector.
    // Reading a dangling reference on the next iteration shows up as
    // garbled UTF-8 in the fetch URL.
    const std::string url = entries[index].url;
    auto includes = ConfigLoader::peekIncludes(url);
    for (const auto &rel : includes) {
        enqueue(resolveIncludeUrl(url, rel), Kind::Include);
    }
}

void UrlAssetSource::Impl::checkDone() {
    if (state == LoadState::Error) {
        return;
    }
    for (const auto &e : entries) {
        if (!e.done || e.failed) {
            return;
        }
    }
    state = LoadState::Ready;
}

UrlAssetSource::UrlAssetSource() : impl_(std::make_unique<Impl>()) {}
UrlAssetSource::~UrlAssetSource() = default;

void UrlAssetSource::start(std::string config_url, std::string input_url, std::string asset_base) {
    config_url = normalizeMemfsPath(config_url);
    input_url = normalizeMemfsPath(input_url);
    // Strip a trailing slash so `asset_base + url` doesn't double up the
    // separator (url already starts with `/`).
    while (!asset_base.empty() && asset_base.back() == '/') {
        asset_base.pop_back();
    }
    impl_->config_path = config_url;
    impl_->input_path = input_url;

    impl_->asset_base = std::move(asset_base);
    impl_->state = LoadState::Fetching;
    impl_->enqueue(config_url, Impl::Kind::Config);
    impl_->enqueue(input_url, Impl::Kind::Input);
}

void UrlAssetSource::poll() {}

LoadState UrlAssetSource::state() const { return impl_->state; }

std::span<const AssetProgress> UrlAssetSource::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

const std::string &UrlAssetSource::errorMessage() const { return impl_->error; }
const std::filesystem::path &UrlAssetSource::configPath() const { return impl_->config_path; }
const std::filesystem::path &UrlAssetSource::inputPath() const { return impl_->input_path; }

} // namespace nodehammer::viewer

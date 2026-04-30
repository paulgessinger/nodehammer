#include <nodehammer/viewer/url_project_fs.hpp>

#include <emscripten/fetch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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

    /// Hierarchical tree snapshot for `list()`. URL keys can include
    /// `/`-separated path components (e.g. `subdir/common.toml`); we
    /// expose them as a real tree so the App's tree panel renders
    /// folders properly. Implicit `DirNode`s are synthesised for any
    /// directory prefix that doesn't have its own fetched entry.
    /// BFS layout — siblings of a directory are contiguous in `tree`
    /// before any of its descendants — so a contiguous span over a
    /// directory's children doesn't accidentally include grandchildren.
    mutable std::vector<DirNode> tree;
    mutable std::unordered_map<std::string, std::size_t> tree_index;
    mutable std::size_t root_first_child{0};
    mutable std::size_t root_child_count{0};
    mutable std::uint64_t tree_snapshot_gen{static_cast<std::uint64_t>(-1)};

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

namespace {

constexpr std::size_t kRootSentinel = static_cast<std::size_t>(-1);

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

std::string parentKey(std::string_view key) {
    auto pos = key.find_last_of('/');
    if (pos == std::string_view::npos) {
        return {};
    }
    return std::string{key.substr(0, pos)};
}

std::string baseKey(std::string_view key) {
    auto pos = key.find_last_of('/');
    if (pos == std::string_view::npos) {
        return std::string{key};
    }
    return std::string{key.substr(pos + 1)};
}

} // namespace

std::span<const DirNode> UrlProjectFs::list(std::string_view dir) const {
    if (impl_->tree_snapshot_gen != impl_->generation) {
        // Rebuild the hierarchical snapshot. URL keys carry path
        // components (e.g. `subdir/common.toml`); we synthesise
        // implicit DirNodes for each directory prefix that doesn't
        // have its own fetched entry, then lay everything out in BFS
        // order so a directory's children occupy a contiguous range.
        impl_->tree.clear();
        impl_->tree_index.clear();
        impl_->root_first_child = 0;
        impl_->root_child_count = 0;

        struct ChildInfo {
            std::string name;
            std::string key;
            bool is_directory;
            std::uint64_t bytes;
        };
        std::unordered_map<std::string, std::vector<ChildInfo>> children_by_parent;
        std::set<std::string> dir_keys;

        // Register every directory prefix of every fetched key as an
        // implicit directory.
        for (const auto &raw : impl_->order) {
            std::string current{stripLeadingSlash(raw)};
            std::string p = parentKey(current);
            while (!p.empty()) {
                dir_keys.insert(p);
                p = parentKey(p);
            }
        }

        // Populate children_by_parent. Files first (with bytes), then
        // implicit directories — sort happens after so order doesn't
        // matter here.
        for (const auto &raw : impl_->order) {
            const std::string key{stripLeadingSlash(raw)};
            const auto &e = *impl_->entries.at(raw);
            children_by_parent[parentKey(key)].push_back(
                ChildInfo{baseKey(key), key, false, e.bytes_total});
        }
        for (const auto &dk : dir_keys) {
            children_by_parent[parentKey(dk)].push_back(ChildInfo{baseKey(dk), dk, true, 0});
        }
        for (auto &[parent, kids] : children_by_parent) {
            std::sort(kids.begin(), kids.end(),
                      [](const auto &a, const auto &b) { return a.name < b.name; });
        }

        struct ChildRange {
            std::size_t parent_idx;
            std::size_t first_child_idx;
            std::size_t child_count;
        };
        std::vector<ChildRange> ranges;

        struct DirJob {
            std::string key; // empty for the root
            std::size_t parent_idx;
        };
        std::deque<DirJob> queue;
        queue.push_back({std::string{}, kRootSentinel});

        while (!queue.empty()) {
            const auto job = queue.front();
            queue.pop_front();

            const auto first_child_idx = impl_->tree.size();
            std::size_t child_count = 0;
            if (auto it = children_by_parent.find(job.key); it != children_by_parent.end()) {
                for (const auto &info : it->second) {
                    DirNode node;
                    node.name = info.name;
                    node.key = info.key;
                    node.is_directory = info.is_directory;
                    node.bytes = info.bytes;
                    const auto idx = impl_->tree.size();
                    impl_->tree.push_back(std::move(node));
                    impl_->tree_index.emplace(impl_->tree.back().key, idx);
                    ++child_count;
                    if (info.is_directory) {
                        queue.push_back({info.key, idx});
                    }
                }
            }
            ranges.push_back({job.parent_idx, first_child_idx, child_count});
        }

        for (const auto &r : ranges) {
            if (r.parent_idx == kRootSentinel) {
                impl_->root_first_child = r.first_child_idx;
                impl_->root_child_count = r.child_count;
            } else if (r.child_count > 0) {
                impl_->tree[r.parent_idx].children =
                    std::span<const DirNode>{impl_->tree.data() + r.first_child_idx, r.child_count};
            }
        }

        impl_->tree_snapshot_gen = impl_->generation;
    }

    if (dir.empty() || dir == "/") {
        if (impl_->root_child_count == 0) {
            return {};
        }
        return {impl_->tree.data() + impl_->root_first_child, impl_->root_child_count};
    }
    const std::string norm{stripLeadingSlash(dir)};
    auto it = impl_->tree_index.find(norm);
    if (it == impl_->tree_index.end()) {
        return {};
    }
    const auto &node = impl_->tree[it->second];
    if (!node.is_directory) {
        return {};
    }
    return node.children;
}

} // namespace nodehammer::viewer

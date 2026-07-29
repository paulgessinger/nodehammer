#include <viewer/bag_project_fs.hpp>

#include <detail/file_io.hpp>

#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string asciiLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(asciiLower(c));
    }
    return out;
}

} // namespace

struct BagProjectFs::Impl {
    /// Bytes-by-key storage. Keys are case-folded filenames so drops with
    /// mismatched casing line up with config includes that use a canonical
    /// spelling. Values are `ByteBuffer` handles — `resolve` returns a
    /// copy (refcount bump). Replacement on `addBytes` swaps in a new
    /// handle; consumers holding the previous one keep their bytes alive.
    std::unordered_map<std::string, ByteBuffer> bytes_by_key;

    /// Per-file display entries, in addition order. The bag debug panel
    /// renders this; App scans it to identify root config + geometry.
    std::vector<ProjectProgress> entries;

    /// Soft warnings (replaced-on-collision). Cleared when a fresh batch
    /// of unrelated drops makes the message stale would be nice but isn't
    /// urgent — keep simple, append-only.
    std::vector<std::string> warning_msgs;

    /// Sticky-once-set: any error during `addPath` flips this true so
    /// `status()` reports Error. The actual message is pushed to the
    /// `LogSink` at the moment of failure; we don't retain it here.
    bool errored{false};

    /// Bumps on every mutation so consumers (App's recognition, the
    /// build-trigger gate) can detect content changes via a single
    /// integer comparison.
    std::uint64_t generation{0};

    /// Flat tree snapshot exposed via `list()`. The bag has no real
    /// hierarchy, so every entry surfaces as a top-level leaf. Rebuilt
    /// lazily on first `list()` after a generation bump; mutable so
    /// the const accessor can refresh without forcing the App to call
    /// a non-const refresh hook.
    mutable std::vector<DirNode> tree_snapshot;
    mutable std::uint64_t tree_snapshot_gen{static_cast<std::uint64_t>(-1)};

    void recordOrReplaceEntry(std::string_view filename, std::size_t size) {
        const auto key = asciiLowerCopy(filename);
        for (auto &p : entries) {
            if (asciiLowerCopy(p.url) == key) {
                p.url = std::string{filename};
                p.bytes_total = static_cast<std::uint64_t>(size);
                p.bytes_done = p.bytes_total;
                p.done = true;
                p.failed = false;
                return;
            }
        }
        ProjectProgress p;
        p.url = std::string{filename};
        p.done = true;
        p.bytes_total = static_cast<std::uint64_t>(size);
        p.bytes_done = p.bytes_total;
        entries.push_back(std::move(p));
    }

    /// Returns the duplicate-replaced warning string when `filename`
    /// collides with an existing entry, empty otherwise. The caller pushes
    /// it through the base helper since `Impl` has no `ProjectFs` access.
    std::string store(std::string_view filename, std::vector<std::byte> bytes) {
        const auto key = asciiLowerCopy(filename);
        std::string warn;
        if (auto it = bytes_by_key.find(key); it != bytes_by_key.end()) {
            warn = "replaced " + std::string{filename};
            warning_msgs.push_back(warn);
        }
        bytes_by_key[key] = ByteBuffer{std::move(bytes)};
        return warn;
    }

    std::string store(std::string_view filename, std::span<const std::byte> bytes) {
        return store(filename, std::vector<std::byte>{bytes.begin(), bytes.end()});
    }
};

BagProjectFs::BagProjectFs() : impl_(std::make_unique<Impl>()) {}
BagProjectFs::~BagProjectFs() = default;

void BagProjectFs::poll() {}

ProjectFsStatus BagProjectFs::status() const {
    return impl_->errored ? ProjectFsStatus::Error : ProjectFsStatus::Idle;
}

std::span<const ProjectProgress> BagProjectFs::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

std::span<const std::string> BagProjectFs::warnings() const {
    return {impl_->warning_msgs.data(), impl_->warning_msgs.size()};
}

std::uint64_t BagProjectFs::generation() const { return impl_->generation; }

ProjectDropDecision BagProjectFs::planAddPath(const std::filesystem::path &path) const {
    return planAddBytes(path.filename().string(), {});
}

ProjectDropDecision BagProjectFs::planAddBytes(std::string_view filename,
                                               std::span<const std::byte> /*bytes*/) const {
    using enum ProjectDropDecision::Kind;
    if (filename.empty()) {
        return ProjectDropDecision{
            Reject, "Cannot add file", "Dropped files must have a filename.", "OK", {},
        };
    }
    if (!impl_->bytes_by_key.contains(asciiLowerCopy(filename))) {
        return ProjectDropDecision{Accept, {}, {}, {}, {}};
    }
    return ProjectDropDecision{
        Confirm,
        "Replace existing file?",
        "A file named \"" + std::string{filename} +
            "\" already exists in this project.\n\nReplace it with the newly added file?",
        "Replace",
        "Cancel",
    };
}

std::span<const DirNode> BagProjectFs::list(std::string_view dir) const {
    if (!dir.empty() && dir != "/") {
        return {};
    }
    if (impl_->tree_snapshot_gen != impl_->generation) {
        impl_->tree_snapshot.clear();
        impl_->tree_snapshot.reserve(impl_->entries.size());
        for (const auto &p : impl_->entries) {
            DirNode n;
            auto base = std::filesystem::path{p.url}.filename().string();
            n.name = base.empty() ? p.url : base;
            n.key = p.url;
            n.is_directory = false;
            n.bytes = p.bytes_total;
            impl_->tree_snapshot.push_back(std::move(n));
        }
        impl_->tree_snapshot_gen = impl_->generation;
    }
    return {impl_->tree_snapshot.data(), impl_->tree_snapshot.size()};
}

ResolveResult BagProjectFs::resolve(std::string_view key) const {
    const auto exact = asciiLowerCopy(key);
    if (auto it = impl_->bytes_by_key.find(exact); it != impl_->bytes_by_key.end()) {
        return ResolveResult{
            ResolveStatus::Ready, OpenedFile{std::string{key}, it->second}, {}, {}};
    }
    // Subdir fallback: the bag has no directory structure today, so
    // includes like `subdir/foo.toml` collapse to their basename. Stage 3+
    // folder drops will populate relative-path keys directly so this
    // fallback becomes rarely-used.
    auto basename = std::filesystem::path{key}.filename().string();
    if (basename != std::string{key}) {
        const auto fb = asciiLowerCopy(basename);
        if (auto it = impl_->bytes_by_key.find(fb); it != impl_->bytes_by_key.end()) {
            return ResolveResult{
                ResolveStatus::Ready, OpenedFile{std::string{key}, it->second}, {}, {}};
        }
    }
    return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
}

void BagProjectFs::addBytes(std::string_view filename, std::span<const std::byte> bytes) {
    if (filename.empty()) {
        return;
    }
    auto warn = impl_->store(filename, bytes);
    if (!warn.empty()) {
        pushWarning(std::move(warn));
    }
    impl_->recordOrReplaceEntry(filename, bytes.size());
    ++impl_->generation;
}

void BagProjectFs::addPath(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }
    std::vector<std::byte> contents;
    try {
        contents = file_io::readFile(path);
    } catch (const std::exception &e) {
        pushError(std::string{"failed to read "} + path.string() + ": " + e.what());
        impl_->errored = true;
        return;
    }
    const auto filename = path.filename().string();
    const auto size = contents.size();
    auto warn = impl_->store(filename, std::move(contents));
    if (!warn.empty()) {
        pushWarning(std::move(warn));
    }
    impl_->recordOrReplaceEntry(filename, size);
    ++impl_->generation;
}

} // namespace nodehammer::viewer

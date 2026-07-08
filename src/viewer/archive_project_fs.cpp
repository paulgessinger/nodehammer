#include <nodehammer/viewer/archive_project_fs.hpp>

#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/viewer/zip_working_set.hpp>

#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nodehammer::viewer {

namespace {

/// Normalise a resolve key to the archive's forward-slash form: drop a leading
/// "/", collapse "." segments, and reject any "..". Returns nullopt for keys
/// that escape the archive root.
std::optional<std::string> normaliseKey(std::string_view key) {
    auto rel = std::filesystem::path{key}.lexically_normal();
    for (const auto &seg : rel) {
        if (seg == "..") {
            return std::nullopt;
        }
    }
    auto s = rel.generic_string();
    if (!s.empty() && s.front() == '/') {
        s.erase(s.begin());
    }
    return s;
}

/// Normalised directory key for the list cache: empty for root, else the
/// forward-slash lexically-normal form with no leading/trailing slash.
std::string normaliseDirKey(std::string_view dir) {
    if (dir.empty() || dir == "/") {
        return {};
    }
    auto s = std::filesystem::path{dir}.lexically_normal().generic_string();
    if (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

/// Serialize-and-swap: write `bytes` to `target` durably. Writes a sibling
/// temp file, fsyncs it, renames over the target, and fsyncs the directory so
/// the rename survives a crash. POSIX-only durability; on Windows we fall back
/// to a best-effort replace.
bool atomicWrite(const std::filesystem::path &target, std::span<const std::byte> bytes,
                 std::string &err) {
    std::filesystem::path tmp = target;
    tmp += ".nhtmp";

    try {
        file_io::writeFile(tmp, bytes);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }

#if !defined(_WIN32)
    if (int fd = ::open(tmp.c_str(), O_RDONLY); fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        // Windows rename fails if the target exists; retry after removing it.
        std::error_code rm_ec;
        std::filesystem::remove(target, rm_ec);
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::error_code cleanup_ec;
            std::filesystem::remove(tmp, cleanup_ec);
            err = ec.message();
            return false;
        }
    }

#if !defined(_WIN32)
    if (int dfd = ::open(target.parent_path().c_str(), O_RDONLY); dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
#endif
    return true;
}

} // namespace

struct ArchiveProjectFs::Impl {
    std::filesystem::path archive_path;
    std::optional<ZipWorkingSet> ws;
    bool errored{false};
    std::string error_msg;
    std::vector<std::string> warning_msgs;

    std::uint64_t generation{0};

    /// Per-directory listing cache, keyed by normalised dir key (empty = root).
    /// Cleared en masse on any generation bump.
    mutable std::unordered_map<std::string, std::vector<DirNode>> dir_cache;
    mutable std::mutex dir_cache_mu;

    void invalidateListing() {
        {
            std::lock_guard<std::mutex> lk(dir_cache_mu);
            dir_cache.clear();
        }
        ++generation;
    }
};

ArchiveProjectFs::ArchiveProjectFs(std::filesystem::path path) : impl_(std::make_unique<Impl>()) {
    impl_->archive_path = std::move(path);
    try {
        impl_->ws = ZipWorkingSet::openFromFile(impl_->archive_path);
    } catch (const std::exception &e) {
        impl_->errored = true;
        impl_->error_msg =
            "failed to open archive " + impl_->archive_path.string() + ": " + e.what();
        pushError(impl_->error_msg);
    }
    ++impl_->generation;
}

ArchiveProjectFs::~ArchiveProjectFs() = default;

void ArchiveProjectFs::poll() {}

ProjectFsStatus ArchiveProjectFs::status() const {
    return (impl_->errored || !impl_->ws) ? ProjectFsStatus::Error : ProjectFsStatus::Ready;
}

std::span<const ProjectProgress> ArchiveProjectFs::progress() const { return {}; }

std::span<const std::string> ArchiveProjectFs::warnings() const {
    return {impl_->warning_msgs.data(), impl_->warning_msgs.size()};
}

ResolveResult ArchiveProjectFs::resolve(std::string_view key) const {
    if (!impl_->ws) {
        return ResolveResult{ResolveStatus::Error,
                             {},
                             std::string{key},
                             impl_->error_msg.empty() ? "archive unavailable" : impl_->error_msg};
    }
    auto norm = normaliseKey(key);
    if (!norm) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }
    auto bytes = impl_->ws->read(*norm);
    if (!bytes) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }
    return ResolveResult{
        ResolveStatus::Ready, OpenedFile{std::string{key}, std::move(*bytes)}, {}, {}};
}

std::uint64_t ArchiveProjectFs::generation() const { return impl_->generation; }

std::span<const DirNode> ArchiveProjectFs::list(std::string_view dir) const {
    if (!impl_->ws) {
        return {};
    }
    auto norm = normaliseDirKey(dir);

    std::lock_guard<std::mutex> lk(impl_->dir_cache_mu);
    if (auto it = impl_->dir_cache.find(norm); it != impl_->dir_cache.end()) {
        return {it->second.data(), it->second.size()};
    }

    std::vector<DirNode> nodes;
    for (auto &e : impl_->ws->listAtPrefix(norm)) {
        nodes.push_back(DirNode{std::move(e.name), std::move(e.key), e.is_directory, e.bytes});
    }
    auto [ins, _] = impl_->dir_cache.emplace(std::move(norm), std::move(nodes));
    return {ins->second.data(), ins->second.size()};
}

void ArchiveProjectFs::rescan() {
    // Archives are app-owned while open (external changes are out of scope), so
    // rescan just drops the listing cache and bumps generation; working-set
    // overrides are preserved.
    impl_->invalidateListing();
}

ProjectDropDecision ArchiveProjectFs::planAddPath(const std::filesystem::path &path) const {
    return planAddBytes(path.filename().string(), {});
}

ProjectDropDecision ArchiveProjectFs::planAddBytes(std::string_view filename,
                                                   std::span<const std::byte> /*bytes*/) const {
    using enum ProjectDropDecision::Kind;
    if (filename.empty()) {
        return ProjectDropDecision{
            Reject, "Cannot add file", "Dropped files must have a filename.", "OK", {}};
    }
    if (impl_->ws && impl_->ws->contains(std::string{filename})) {
        return ProjectDropDecision{
            Confirm,
            "Replace existing file?",
            "A file named \"" + std::string{filename} +
                "\" already exists in this archive.\n\nReplace it?",
            "Replace",
            "Cancel",
        };
    }
    return ProjectDropDecision{Accept, {}, {}, {}, {}};
}

void ArchiveProjectFs::addBytes(std::string_view filename, std::span<const std::byte> bytes) {
    if (filename.empty() || !impl_->ws) {
        return;
    }
    // Drops land flat at the archive root under their basename; a future editor
    // commit will pass full archive keys directly through this path.
    if (filename.find('/') != std::string_view::npos ||
        filename.find('\\') != std::string_view::npos || filename == "." || filename == "..") {
        return;
    }

    const std::string key{filename};
    const bool replacing = impl_->ws->contains(key);

    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    impl_->ws->writeEntry(key, std::move(copy));

    if (replacing) {
        auto msg = "replaced " + key;
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
    }
    impl_->invalidateListing();
}

void ArchiveProjectFs::addPath(const std::filesystem::path &path) {
    if (path.empty() || !impl_->ws) {
        return;
    }
    std::vector<std::byte> contents;
    try {
        contents = file_io::readFile(path);
    } catch (const std::exception &e) {
        auto msg = std::string{"failed to read "} + path.string() + ": " + e.what();
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
        return;
    }
    addBytes(path.filename().string(), contents);
}

const std::filesystem::path &ArchiveProjectFs::path() const { return impl_->archive_path; }

bool ArchiveProjectFs::dirty() const { return impl_->ws && impl_->ws->dirty(); }

bool ArchiveProjectFs::save() {
    if (!impl_->ws) {
        auto msg = std::string{"cannot save: archive is not open"};
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
        return false;
    }

    std::vector<std::byte> blob;
    try {
        blob = impl_->ws->serialize();
    } catch (const std::exception &e) {
        auto msg = std::string{"failed to serialize archive: "} + e.what();
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
        return false;
    }

    std::string err;
    if (!atomicWrite(impl_->archive_path, blob, err)) {
        auto msg = "failed to write " + impl_->archive_path.string() + ": " + err;
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
        return false;
    }

    // Reopen from the freshly written file so the working set drops its overrides
    // and dirty flag, and subsequent reads come from the saved bytes.
    try {
        impl_->ws = ZipWorkingSet::openFromBytes(blob);
    } catch (const std::exception &e) {
        // The file on disk is fine; only the in-memory view failed to refresh.
        auto msg = std::string{"archive saved, but reopen failed: "} + e.what();
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
    }
    impl_->invalidateListing();
    return true;
}

} // namespace nodehammer::viewer

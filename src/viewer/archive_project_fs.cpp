#include <nodehammer/viewer/archive_project_fs.hpp>

#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/viewer/archive_export.hpp>
#include <nodehammer/viewer/zip_working_set.hpp>

#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

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

} // namespace

struct ArchiveProjectFs::Impl {
    std::filesystem::path archive_path;
    std::optional<ZipWorkingSet> ws;
    Provenance provenance{Provenance::Local};
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

ArchiveProjectFs::ArchiveProjectFs(ZipWorkingSet ws, Provenance provenance)
    : impl_(std::make_unique<Impl>()) {
    // Unbound: no backing file. archive_path stays empty; save() fails until a
    // path is bound via saveTo (native) — web persists by download / IDB.
    impl_->ws = std::move(ws);
    impl_->provenance = provenance;
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
    if (!bytes && impl_->provenance == Provenance::Empty) {
        // Scratch project assembled from flat loose drops (no real directory
        // structure): fall back to the basename so an include like
        // `sub/common.toml` resolves to a root-dropped `common.toml`. Opened
        // archives (Local/Remote) carry full paths and keep strict resolution.
        auto base = std::filesystem::path{*norm}.filename().string();
        if (!base.empty() && base != *norm) {
            bytes = impl_->ws->read(base);
        }
    }
    if (bytes) {
        return ResolveResult{
            ResolveStatus::Ready, OpenedFile{std::string{key}, std::move(*bytes)}, {}, {}};
    }
    // Unresolved: an entry present in the working set that still won't read is
    // corrupt (Error); one that isn't there at all is genuinely Missing.
    if (impl_->ws->contains(*norm)) {
        return ResolveResult{ResolveStatus::Error,
                             {},
                             std::string{key},
                             "failed to read archive entry: " + std::string{key}};
    }
    return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
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

ProjectDropDecision ArchiveProjectFs::planRemove(std::string_view key) const {
    using enum ProjectDropDecision::Kind;
    if (!impl_->ws || key.empty() || !impl_->ws->contains(std::string{key})) {
        return ProjectDropDecision{
            Reject, "Cannot remove file", "That file is not part of this archive.", "OK", {}};
    }
    return ProjectDropDecision{
        Confirm,
        "Remove file?",
        "Remove \"" + std::string{key} + "\" from this archive?\n\nThis cannot be undone.",
        "Remove",
        "Cancel",
    };
}

void ArchiveProjectFs::removeKey(std::string_view key) {
    if (!impl_->ws || key.empty()) {
        return;
    }
    const std::string k{key};
    if (!impl_->ws->contains(k)) {
        return;
    }
    impl_->ws->removeEntry(k);
    impl_->invalidateListing();
}

ProjectDropDecision ArchiveProjectFs::planMove(std::string_view from_key,
                                               std::string_view to_key) const {
    using enum ProjectDropDecision::Kind;
    if (!impl_->ws || from_key.empty() || to_key.empty()) {
        return ProjectDropDecision{
            Reject, "Cannot move file", "Missing source or destination.", "OK", {}};
    }
    if (from_key == to_key) {
        // Already where it would land (dropped onto its own folder) — no-op.
        return ProjectDropDecision{Reject, {}, {}, {}, {}};
    }
    if (!impl_->ws->contains(std::string{from_key})) {
        return ProjectDropDecision{
            Reject, "Cannot move file", "That file is not part of this archive.", "OK", {}};
    }
    if (impl_->ws->contains(std::string{to_key})) {
        return ProjectDropDecision{
            Confirm,
            "Replace existing file?",
            "A file named \"" + std::string{to_key} +
                "\" already exists here.\n\nReplace it with the moved file?",
            "Replace",
            "Cancel",
        };
    }
    return ProjectDropDecision{Accept, {}, {}, {}, {}};
}

void ArchiveProjectFs::moveKey(std::string_view from_key, std::string_view to_key) {
    if (!impl_->ws || from_key.empty() || to_key.empty() || from_key == to_key) {
        return;
    }
    const std::string fromK{from_key};
    const std::string toK{to_key};
    auto bytes = impl_->ws->read(fromK);
    if (!bytes) {
        return; // source vanished between plan and commit
    }
    const auto sp = bytes->span();
    impl_->ws->writeEntry(toK, std::vector<std::byte>(sp.begin(), sp.end()));
    impl_->ws->removeEntry(fromK);
    impl_->invalidateListing();
}

ArchiveProjectFs::Provenance ArchiveProjectFs::provenance() const { return impl_->provenance; }

const std::filesystem::path &ArchiveProjectFs::path() const { return impl_->archive_path; }

bool ArchiveProjectFs::isBound() const { return !impl_->archive_path.empty(); }

bool ArchiveProjectFs::dirty() const { return impl_->ws && impl_->ws->dirty(); }

std::vector<std::byte> ArchiveProjectFs::serialize() const {
    if (!impl_->ws) {
        return {};
    }
    return impl_->ws->serialize();
}

bool ArchiveProjectFs::save() {
#ifdef __EMSCRIPTEN__
    // No filesystem path to write to on web; the App persists via download.
    auto msg = std::string{"cannot save: web archives persist by download"};
    impl_->warning_msgs.push_back(msg);
    pushWarning(std::move(msg));
    return false;
#else
    if (!isBound()) {
        auto msg = std::string{"cannot save: archive has no bound path (use save as)"};
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
        return false;
    }
    return saveTo(impl_->archive_path);
#endif
}

#ifndef __EMSCRIPTEN__

bool ArchiveProjectFs::saveTo(const std::filesystem::path &path) {
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
    if (!writeBytesAtomic(path, blob, err)) {
        auto msg = "failed to write " + path.string() + ": " + err;
        impl_->warning_msgs.push_back(msg);
        pushWarning(std::move(msg));
        return false;
    }

    // Bind to the written path (a no-op when re-saving in place). The on-disk
    // file now matches the in-memory working set verbatim, so just drop the dirty
    // flag — do NOT reopen or bump generation. Saving changes neither the
    // resolvable content nor the listing, so a bump would only force the
    // BuildSession to re-walk → re-import → re-tessellate an identical scene. The
    // menu reads isBound()/dirty() directly each frame, so binding needs no bump.
    impl_->archive_path = path;
    impl_->ws->clearDirty();
    return true;
}

#endif // __EMSCRIPTEN__

} // namespace nodehammer::viewer

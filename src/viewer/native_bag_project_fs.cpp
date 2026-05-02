#include <nodehammer/viewer/native_bag_project_fs.hpp>

#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/viewer/filesystem_project_fs.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace nodehammer::viewer {

namespace {

long long currentPid() {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(::getpid());
#endif
}

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string asciiLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(asciiLower(c));
    }
    return out;
}

/// Allocate a unique storage dir under temp_directory_path(). PID + a
/// process-static counter + a wall-clock nanosecond suffix make collisions
/// vanishingly unlikely without needing a UUID dependency.
std::filesystem::path makeStorageDir() {
    static std::atomic<std::uint64_t> counter{0};
    const auto n = counter.fetch_add(1, std::memory_order_relaxed);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return std::filesystem::temp_directory_path() /
           std::format("nodehammer-bag-{}-{}-{}", currentPid(), n, ns);
}

} // namespace

struct NativeBagProjectFs::Impl {
    std::filesystem::path storage_dir;
    std::unique_ptr<FilesystemProjectFs> inner;

    /// Per-file display entries, in addition order. Mirrors the in-memory
    /// bag's `progress()` for the project panel.
    std::vector<ProjectProgress> entries;
    std::vector<std::string> warning_msgs;
    std::string error;

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

    /// If a prior drop wrote the same basename under a different literal
    /// case, remove that file before writing the new one. Prevents two
    /// files coexisting on case-sensitive filesystems after a re-drop.
    void removeStaleCaseVariant(std::string_view filename) const {
        const auto key = asciiLowerCopy(filename);
        for (const auto &p : entries) {
            if (asciiLowerCopy(p.url) == key && p.url != filename) {
                std::error_code ec;
                std::filesystem::remove(storage_dir / p.url, ec);
                return;
            }
        }
    }
};

NativeBagProjectFs::NativeBagProjectFs() : impl_(std::make_unique<Impl>()) {
    impl_->storage_dir = makeStorageDir();
    std::error_code ec;
    std::filesystem::create_directories(impl_->storage_dir, ec);
    if (ec) {
        impl_->error =
            "failed to create bag storage dir " + impl_->storage_dir.string() + ": " + ec.message();
        return;
    }
    impl_->inner = std::make_unique<FilesystemProjectFs>(impl_->storage_dir);
}

NativeBagProjectFs::~NativeBagProjectFs() {
    if (impl_->storage_dir.empty()) {
        return;
    }
    try {
        std::error_code ec;
        std::filesystem::remove_all(impl_->storage_dir, ec);
    } catch (...) {
        // Dtor must not throw; OS temp reaping handles whatever we leak.
    }
}

void NativeBagProjectFs::poll() {
    if (impl_->inner) {
        impl_->inner->poll();
    }
}

ProjectFsStatus NativeBagProjectFs::status() const {
    if (!impl_->error.empty()) {
        return ProjectFsStatus::Error;
    }
    return impl_->inner ? impl_->inner->status() : ProjectFsStatus::Error;
}

std::span<const ProjectProgress> NativeBagProjectFs::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

const std::string &NativeBagProjectFs::errorMessage() const {
    if (!impl_->error.empty()) {
        return impl_->error;
    }
    return impl_->inner ? impl_->inner->errorMessage() : impl_->error;
}

std::span<const std::string> NativeBagProjectFs::warnings() const {
    return {impl_->warning_msgs.data(), impl_->warning_msgs.size()};
}

std::uint64_t NativeBagProjectFs::generation() const {
    return impl_->inner ? impl_->inner->generation() : 0;
}

std::span<const DirNode> NativeBagProjectFs::list(std::string_view dir) const {
    return impl_->inner ? impl_->inner->list(dir) : std::span<const DirNode>{};
}

void NativeBagProjectFs::rescan() {
    if (impl_->inner) {
        impl_->inner->rescan();
    }
}

ResolveResult NativeBagProjectFs::resolve(std::string_view key) const {
    if (!impl_->inner) {
        return ResolveResult{ResolveStatus::Error, {}, std::string{key}, impl_->error};
    }
    auto r = impl_->inner->resolve(key);
    if (r.status != ResolveStatus::Missing) {
        return r;
    }
    // Subdir fallback: includes like `subdir/foo.toml` collapse to the
    // basename when the user dropped `foo.toml` flat at the bag root.
    auto basename = std::filesystem::path{key}.filename().string();
    if (basename.empty() || basename == std::string{key}) {
        return r;
    }
    auto fb = impl_->inner->resolve(basename);
    if (fb.status == ResolveStatus::Ready) {
        // Re-key the result so callers see the key they asked for.
        fb.file.key = std::string{key};
    }
    return fb;
}

ProjectDropDecision NativeBagProjectFs::planAddPath(const std::filesystem::path &path) const {
    return planAddBytes(path.filename().string(), {});
}

ProjectDropDecision NativeBagProjectFs::planAddBytes(std::string_view filename,
                                                     std::span<const std::byte> /*bytes*/) const {
    using enum ProjectDropDecision::Kind;
    if (filename.empty()) {
        return ProjectDropDecision{
            Reject, "Cannot add file", "Dropped files must have a filename.", "OK", {},
        };
    }
    const auto key = asciiLowerCopy(filename);
    bool exists = false;
    for (const auto &p : impl_->entries) {
        if (asciiLowerCopy(p.url) == key) {
            exists = true;
            break;
        }
    }
    if (!exists) {
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

void NativeBagProjectFs::addBytes(std::string_view filename, std::span<const std::byte> bytes) {
    if (filename.empty() || !impl_->inner) {
        return;
    }
    // Reject anything that looks like a path: native bag is flat-content
    // by design, matching the in-memory bag.
    if (filename.find('/') != std::string_view::npos ||
        filename.find('\\') != std::string_view::npos || filename == "." || filename == "..") {
        return;
    }

    const auto key = asciiLowerCopy(filename);
    bool replacing = false;
    for (const auto &p : impl_->entries) {
        if (asciiLowerCopy(p.url) == key) {
            replacing = true;
            break;
        }
    }
    impl_->removeStaleCaseVariant(filename);

    const auto target = impl_->storage_dir / std::filesystem::path{filename};
    try {
        file_io::writeFile(target, bytes);
    } catch (const std::exception &e) {
        impl_->error = std::string{"failed to write "} + target.string() + ": " + e.what();
        return;
    }
    if (replacing) {
        impl_->warning_msgs.emplace_back("replaced " + std::string{filename});
    }
    impl_->recordOrReplaceEntry(filename, bytes.size());
    impl_->inner->rescan();
}

void NativeBagProjectFs::addPath(const std::filesystem::path &path) {
    if (path.empty() || !impl_->inner) {
        return;
    }
    std::vector<std::byte> contents;
    try {
        contents = file_io::readFile(path);
    } catch (const std::exception &e) {
        impl_->error = std::string{"failed to read "} + path.string() + ": " + e.what();
        return;
    }
    addBytes(path.filename().string(), contents);
}

const std::filesystem::path &NativeBagProjectFs::storageDir() const { return impl_->storage_dir; }

} // namespace nodehammer::viewer

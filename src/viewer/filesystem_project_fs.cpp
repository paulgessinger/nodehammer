#include <viewer/filesystem_project_fs.hpp>

#include <detail/file_io.hpp>

#include <algorithm>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

/// Forward-slash relative key from `path` to `root`. Empty for `path == root`.
std::string makeKey(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto rel = path.lexically_relative(root);
    return rel.generic_string();
}

/// Normalised cache key for a directory: empty for root, otherwise
/// `lexically_normal().generic_string()`. Trailing slashes collapse, `.`
/// segments disappear; we reject any input that escapes the project root
/// via `..` separately.
std::string normaliseDirKey(std::string_view dir) {
    if (dir.empty() || dir == "/") {
        return {};
    }
    return std::filesystem::path{dir}.lexically_normal().generic_string();
}

/// Returns true when `rel` (a lexically-normal relative path) does not
/// escape the project root via `..`. Used by `list()` and `resolve()` to
/// reject keys like `../../etc/passwd` before any disk access.
bool isWithinRoot(const std::filesystem::path &rel) {
    for (const auto &seg : rel) {
        if (seg == "..") {
            return false;
        }
    }
    return true;
}

/// Returns true when any path segment of `rel` starts with `.` — used by
/// `resolve()` under `skip_hidden_files` to make dot-prefixed entries
/// behave as if they don't exist (matching the listing filter).
bool hasHiddenSegment(const std::filesystem::path &rel) {
    for (const auto &seg : rel) {
        const auto s = seg.string();
        if (!s.empty() && s.front() == '.' && s != ".") {
            return true;
        }
    }
    return false;
}

} // namespace

struct FilesystemProjectFs::Impl {
    std::filesystem::path root;
    FilesystemProjectFs::Options options{};

    /// Soft warnings (e.g. addPath/addBytes-on-filesystem hint).
    std::vector<std::string> warning_msgs;

    /// Bumps on construction and on every rescan. Each bump invalidates
    /// `dir_cache` and `byte_cache` spans handed out from earlier calls.
    std::uint64_t generation{0};

    /// Per-directory listing cache. Key is the normalised directory key
    /// (empty for root). Spans returned from `list()` point into the
    /// stored vectors. Cleared en masse on `rescan()`.
    mutable std::unordered_map<std::string, std::vector<DirNode>> dir_cache;
    mutable std::mutex dir_cache_mu;

    /// Walk `root / dir_key` once and return its immediate children
    /// (directories + regular files), sorted alphabetically. Hidden
    /// entries are filtered per `options.skip_hidden_files`.
    std::vector<DirNode> walkDir(const std::string &dir_key) const;
};

std::vector<DirNode> FilesystemProjectFs::Impl::walkDir(const std::string &dir_key) const {
    std::vector<DirNode> out;

    std::filesystem::path abs = root;
    if (!dir_key.empty()) {
        abs /= std::filesystem::path{dir_key};
    }

    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(abs, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.path() < b.path(); });

    out.reserve(entries.size());
    for (const auto &entry : entries) {
        const auto &p = entry.path();

        if (options.skip_hidden_files) {
            const auto fname = p.filename().string();
            if (!fname.empty() && fname.front() == '.') {
                continue;
            }
        }

        std::error_code is_dir_ec;
        const bool is_dir = entry.is_directory(is_dir_ec);
        std::error_code is_reg_ec;
        const bool is_reg = entry.is_regular_file(is_reg_ec);
        if (!is_dir && !is_reg) {
            continue; // skip symlinks / sockets / unknown
        }

        DirNode node;
        node.name = p.filename().string();
        node.key = makeKey(root, p);
        node.is_directory = is_dir;
        if (is_reg) {
            std::error_code size_ec;
            const auto size = entry.file_size(size_ec);
            node.bytes = size_ec ? 0 : static_cast<std::uint64_t>(size);
        }
        out.push_back(std::move(node));
    }
    return out;
}

FilesystemProjectFs::FilesystemProjectFs(const std::filesystem::path &root)
    : FilesystemProjectFs(root, Options{}) {}

FilesystemProjectFs::FilesystemProjectFs(const std::filesystem::path &root, Options options)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    std::error_code ec;
    impl_->root = std::filesystem::canonical(root, ec);
    if (ec) {
        // Fall back to absolute() if canonical fails (e.g. permission denied
        // on a parent component). Walk will still error out cleanly via
        // the iterator's error_code if root is inaccessible.
        impl_->root = std::filesystem::absolute(root);
    }
    ++impl_->generation;
}

FilesystemProjectFs::~FilesystemProjectFs() = default;

void FilesystemProjectFs::poll() {}

ProjectFsStatus FilesystemProjectFs::status() const { return ProjectFsStatus::Ready; }

std::span<const ProjectProgress> FilesystemProjectFs::progress() const { return {}; }

std::span<const std::string> FilesystemProjectFs::warnings() const {
    return {impl_->warning_msgs.data(), impl_->warning_msgs.size()};
}

ProjectDropDecision FilesystemProjectFs::planAddPath(const std::filesystem::path &path) const {
    return ProjectDropDecision{
        ProjectDropDecision::Kind::Reject,
        "Cannot add file to filesystem project",
        "This project is mounted from a folder on disk.\n\nAdd \"" + path.filename().string() +
            "\" to the mounted folder, then click Rescan.",
        "OK",
        {},
    };
}

ProjectDropDecision FilesystemProjectFs::planAddBytes(std::string_view filename,
                                                      std::span<const std::byte> /*bytes*/) const {
    return ProjectDropDecision{
        ProjectDropDecision::Kind::Reject,
        "Cannot add file to filesystem project",
        "This project is mounted from a folder on disk.\n\nAdd \"" + std::string{filename} +
            "\" to the mounted folder, then click Rescan.",
        "OK",
        {},
    };
}

void FilesystemProjectFs::addPath(const std::filesystem::path & /*path*/) {
    auto msg = std::string{"this project is filesystem-mounted; drop new files into the "
                           "folder on disk and click Rescan"};
    impl_->warning_msgs.push_back(msg);
    pushWarning(std::move(msg));
}

void FilesystemProjectFs::addBytes(std::string_view /*filename*/,
                                   std::span<const std::byte> /*bytes*/) {
    auto msg = std::string{
        "this project is filesystem-mounted; addBytes ignored -- write to disk and click Rescan"};
    impl_->warning_msgs.push_back(msg);
    pushWarning(std::move(msg));
}

ResolveResult FilesystemProjectFs::resolve(std::string_view key) const {
    auto rel = std::filesystem::path{key}.lexically_normal();
    if (!isWithinRoot(rel)) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }
    if (impl_->options.skip_hidden_files && hasHiddenSegment(rel)) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }

    auto abs = impl_->root / rel;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(abs, ec)) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }

    std::vector<std::byte> bytes;
    try {
        bytes = file_io::readFile(abs);
    } catch (const std::exception &e) {
        return ResolveResult{ResolveStatus::Error, {}, std::string{key}, e.what()};
    }
    return ResolveResult{
        ResolveStatus::Ready, OpenedFile{std::string{key}, ByteBuffer{std::move(bytes)}}, {}, {}};
}

std::uint64_t FilesystemProjectFs::generation() const { return impl_->generation; }

std::span<const DirNode> FilesystemProjectFs::list(std::string_view dir) const {
    auto norm = normaliseDirKey(dir);
    if (!norm.empty() && !isWithinRoot(std::filesystem::path{norm})) {
        return {};
    }

    std::lock_guard<std::mutex> lk(impl_->dir_cache_mu);
    if (auto it = impl_->dir_cache.find(norm); it != impl_->dir_cache.end()) {
        return {it->second.data(), it->second.size()};
    }

    // Reject non-root keys whose target isn't an actual directory; root
    // is always considered a directory (we may still get an empty span if
    // it's empty or unreadable).
    if (!norm.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(impl_->root / std::filesystem::path{norm}, ec)) {
            return {};
        }
    }

    auto entries = impl_->walkDir(norm);
    auto [ins, _] = impl_->dir_cache.emplace(std::move(norm), std::move(entries));
    return {ins->second.data(), ins->second.size()};
}

void FilesystemProjectFs::rescan() {
    {
        std::lock_guard<std::mutex> lk(impl_->dir_cache_mu);
        impl_->dir_cache.clear();
    }
    impl_->warning_msgs.clear();
    ++impl_->generation;
}

const std::filesystem::path &FilesystemProjectFs::root() const { return impl_->root; }

} // namespace nodehammer::viewer

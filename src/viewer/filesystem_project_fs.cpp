#include <nodehammer/viewer/filesystem_project_fs.hpp>

#include <nodehammer/detail/file_io.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
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

constexpr std::size_t kRootSentinel = static_cast<std::size_t>(-1);

} // namespace

struct FilesystemProjectFs::Impl {
    std::filesystem::path root;
    FilesystemProjectFs::Options options{};

    /// Flat owning storage for the directory snapshot. Built once in
    /// `buildTree()`; child spans inside each `DirNode` point into
    /// contiguous slices of this vector.
    std::vector<DirNode> tree;

    /// O(1) key → tree index for `resolve` and `list(dir)` lookups.
    std::unordered_map<std::string, std::size_t> tree_index;

    /// Top-level (root) child range. The root itself isn't represented
    /// as a `DirNode` in `tree`, so we cache its child slice separately
    /// and expose it via `list("")`.
    std::size_t root_first_child{0};
    std::size_t root_child_count{0};

    /// Per-file progress. Lets the existing flat progress UI render a
    /// sensible summary; the tree panel reads `tree` instead.
    std::vector<ProjectProgress> progress_entries;

    /// Soft warnings (e.g. addPath/addBytes-on-filesystem hint).
    std::vector<std::string> warning_msgs;

    std::string error;

    /// Bumps on construction and on every rescan.
    std::uint64_t generation{0};

    /// Lazily-populated byte cache. Spans handed out from `resolve`
    /// point into these vectors, valid for the lifetime of a generation.
    mutable std::unordered_map<std::string, std::vector<std::byte>> byte_cache;
    mutable std::mutex byte_cache_mu;

    /// Walk `root` and rebuild `tree` + `tree_index` + `progress_entries`.
    /// Caller is responsible for clearing `byte_cache` and bumping
    /// `generation`.
    void buildTree();
};

void FilesystemProjectFs::Impl::buildTree() {
    tree.clear();
    tree_index.clear();
    progress_entries.clear();
    root_first_child = 0;
    root_child_count = 0;

    // BFS layout: process directories level by level so a directory's
    // direct children land in a contiguous range of `tree`. Pre-order
    // DFS would interleave grandchildren between siblings (visit a
    // subdir, then its descendants, only THEN the subdir's
    // alphabetical sibling), making any contiguous-span representation
    // of a parent's children wrong. With BFS, when we finish enqueuing
    // the children of a directory, those children occupy
    // `[first_child_idx, first_child_idx + child_count)` in `tree`.
    // tree.data() may move as we keep pushing more nodes, so we
    // record (parent_idx, first_child_idx, child_count) per directory
    // and write the spans in a second pass once `tree` is finalised.

    auto entries_in_dir = [](const std::filesystem::path &dir) {
        std::vector<std::filesystem::directory_entry> out;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            out.push_back(*it);
        }
        std::sort(out.begin(), out.end(),
                  [](const auto &a, const auto &b) { return a.path() < b.path(); });
        return out;
    };

    struct ChildRange {
        std::size_t parent_idx; // kRootSentinel for top-level
        std::size_t first_child_idx;
        std::size_t child_count;
    };
    std::vector<ChildRange> ranges;

    struct DirJob {
        std::filesystem::path dir;
        std::size_t parent_idx; // kRootSentinel for top-level
    };
    std::deque<DirJob> queue;
    queue.push_back({root, kRootSentinel});

    while (!queue.empty()) {
        const auto job = queue.front();
        queue.pop_front();

        const auto entries = entries_in_dir(job.dir);
        const std::size_t first_child_idx = tree.size();
        std::size_t child_count = 0;

        for (const auto &entry : entries) {
            const auto &p = entry.path();

            // Skip dot-prefixed entries (.DS_Store, .git, editor swap
            // files, ...) when the option is on. Applied at every
            // depth so a hidden directory is pruned wholesale.
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

            const std::size_t idx = tree.size();
            tree.push_back(std::move(node));
            tree_index.emplace(tree.back().key, idx);
            ++child_count;

            if (is_reg) {
                ProjectProgress prog;
                prog.url = tree.back().key;
                prog.done = true;
                prog.bytes_total = tree.back().bytes;
                prog.bytes_done = tree.back().bytes;
                progress_entries.push_back(std::move(prog));
            } else {
                queue.push_back({p, idx});
            }
        }

        ranges.push_back({job.parent_idx, first_child_idx, child_count});
    }

    // Second pass: write child spans into directory nodes (and the
    // root sentinel). `tree.data()` is now stable.
    for (const auto &r : ranges) {
        if (r.parent_idx == kRootSentinel) {
            root_first_child = r.first_child_idx;
            root_child_count = r.child_count;
        } else if (r.child_count > 0) {
            tree[r.parent_idx].children =
                std::span<const DirNode>{tree.data() + r.first_child_idx, r.child_count};
        }
    }
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
    impl_->buildTree();
    ++impl_->generation;
}

FilesystemProjectFs::~FilesystemProjectFs() = default;

void FilesystemProjectFs::poll() {}

ProjectFsStatus FilesystemProjectFs::status() const {
    if (!impl_->error.empty()) {
        return ProjectFsStatus::Error;
    }
    return ProjectFsStatus::Ready;
}

std::span<const ProjectProgress> FilesystemProjectFs::progress() const {
    return {impl_->progress_entries.data(), impl_->progress_entries.size()};
}

const std::string &FilesystemProjectFs::errorMessage() const { return impl_->error; }

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
    impl_->warning_msgs.emplace_back("this project is filesystem-mounted; drop new files into the "
                                     "folder on disk and click Rescan");
}

void FilesystemProjectFs::addBytes(std::string_view /*filename*/,
                                   std::span<const std::byte> /*bytes*/) {
    impl_->warning_msgs.emplace_back(
        "this project is filesystem-mounted; addBytes ignored — write to disk and click Rescan");
}

ResolveResult FilesystemProjectFs::resolve(std::string_view key) const {
    // Normalise: collapse any `subdir/../foo` indirection. The
    // BuildSession already normalises, but defense in depth is cheap.
    auto norm = std::filesystem::path{key}.lexically_normal().generic_string();

    auto it = impl_->tree_index.find(norm);
    if (it == impl_->tree_index.end()) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }
    const auto &node = impl_->tree[it->second];
    if (node.is_directory) {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }

    std::lock_guard<std::mutex> lk(impl_->byte_cache_mu);
    if (auto cached = impl_->byte_cache.find(norm); cached != impl_->byte_cache.end()) {
        return ResolveResult{
            ResolveStatus::Ready,
            OpenedFile{std::string{key}, std::span<const std::byte>{cached->second}},
            {},
            {}};
    }

    std::vector<std::byte> bytes;
    try {
        bytes = file_io::readFile(impl_->root / std::filesystem::path{norm});
    } catch (const std::exception &e) {
        return ResolveResult{ResolveStatus::Error, {}, std::string{key}, e.what()};
    }
    auto [ins, _] = impl_->byte_cache.emplace(norm, std::move(bytes));
    return ResolveResult{ResolveStatus::Ready,
                         OpenedFile{std::string{key}, std::span<const std::byte>{ins->second}},
                         {},
                         {}};
}

std::uint64_t FilesystemProjectFs::generation() const { return impl_->generation; }

std::span<const DirNode> FilesystemProjectFs::list(std::string_view dir) const {
    if (dir.empty() || dir == "/") {
        if (impl_->root_child_count == 0) {
            return {};
        }
        return {impl_->tree.data() + impl_->root_first_child, impl_->root_child_count};
    }
    auto norm = std::filesystem::path{dir}.lexically_normal().generic_string();
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

void FilesystemProjectFs::rescan() {
    {
        std::lock_guard<std::mutex> lk(impl_->byte_cache_mu);
        impl_->byte_cache.clear();
    }
    impl_->warning_msgs.clear();
    impl_->error.clear();
    impl_->buildTree();
    ++impl_->generation;
}

const std::filesystem::path &FilesystemProjectFs::root() const { return impl_->root; }

} // namespace nodehammer::viewer

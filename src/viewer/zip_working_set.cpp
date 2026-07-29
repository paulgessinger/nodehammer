#include <viewer/zip_working_set.hpp>

#include <detail/file_io.hpp>

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nodehammer::viewer {

namespace {

/// Normalise a directory prefix to the "" (root) or "dir/subdir/" form used to
/// match entry keys. Empty and "/" both mean root.
std::string dirBase(std::string_view prefix) {
    if (prefix.empty() || prefix == "/") {
        return {};
    }
    std::string base{prefix};
    if (base.back() != '/') {
        base.push_back('/');
    }
    return base;
}

} // namespace

struct ZipWorkingSet::Impl {
    /// Backing store for the reader: mz_zip_reader_init_mem does not copy, so
    /// these bytes must outlive `reader`.
    std::vector<std::byte> archive_bytes;
    mz_zip_archive reader{};
    bool reader_ok{false};

    struct OriginalEntry {
        mz_uint file_index{0};
        std::uint64_t size{0};
    };
    /// File entries present in the archive (directory records excluded), keyed
    /// by their forward-slash name, plus a stable file-index order for
    /// serialize() passthrough.
    std::unordered_map<std::string, OriginalEntry> originals;
    std::vector<std::string> original_order;

    /// Edit overlay.
    std::unordered_map<std::string, ByteBuffer> overrides;
    std::unordered_set<std::string> tombstones;
    /// Decompressed originals, cached on first read.
    std::unordered_map<std::string, ByteBuffer> decompressed;

    bool dirty{false};

    ~Impl() {
        if (reader_ok) {
            mz_zip_reader_end(&reader);
        }
    }
    Impl() = default;
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    bool hasOriginal(const std::string &key) const {
        return originals.find(key) != originals.end();
    }
};

ZipWorkingSet::ZipWorkingSet(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ZipWorkingSet::~ZipWorkingSet() = default;
ZipWorkingSet::ZipWorkingSet(ZipWorkingSet &&) noexcept = default;
ZipWorkingSet &ZipWorkingSet::operator=(ZipWorkingSet &&) noexcept = default;

ZipWorkingSet ZipWorkingSet::create() {
    // No backing archive: the reader stays uninitialised (reader_ok=false) and
    // the originals maps stay empty. read/contains/listAtPrefix/serialize only
    // touch the reader inside the (empty) original_order loops, so a from-scratch
    // set works entirely off its overrides.
    return ZipWorkingSet{std::make_unique<Impl>()};
}

ZipWorkingSet ZipWorkingSet::openFromBytes(std::span<const std::byte> bytes) {
    auto impl = std::make_unique<Impl>();
    impl->archive_bytes.assign(bytes.begin(), bytes.end());

    std::memset(&impl->reader, 0, sizeof(impl->reader));
    if (!mz_zip_reader_init_mem(&impl->reader, impl->archive_bytes.data(),
                                impl->archive_bytes.size(), 0)) {
        throw std::runtime_error("ZipWorkingSet: not a readable ZIP archive");
    }
    impl->reader_ok = true;

    const mz_uint count = mz_zip_reader_get_num_files(&impl->reader);
    impl->original_order.reserve(count);
    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&impl->reader, i)) {
            continue;
        }
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&impl->reader, i, &stat)) {
            continue;
        }
        std::string name{stat.m_filename};
        // Guard against a corrupt archive listing the same name twice: keep the
        // first occurrence (that is what mz_zip_reader_locate_file would find).
        if (impl->originals.emplace(name, Impl::OriginalEntry{i, stat.m_uncomp_size}).second) {
            impl->original_order.push_back(std::move(name));
        }
    }

    return ZipWorkingSet{std::move(impl)};
}

ZipWorkingSet ZipWorkingSet::openFromFile(const std::filesystem::path &path) {
    // Throws std::runtime_error on open failure, matching file_io's contract.
    std::vector<std::byte> bytes = file_io::readFile(path);
    return openFromBytes(bytes);
}

bool ZipWorkingSet::contains(std::string_view key) const {
    std::string k{key};
    if (impl_->overrides.find(k) != impl_->overrides.end()) {
        return true;
    }
    if (impl_->tombstones.find(k) != impl_->tombstones.end()) {
        return false;
    }
    return impl_->hasOriginal(k);
}

std::optional<ByteBuffer> ZipWorkingSet::read(std::string_view key) {
    std::string k{key};

    if (auto it = impl_->overrides.find(k); it != impl_->overrides.end()) {
        return it->second;
    }
    if (impl_->tombstones.find(k) != impl_->tombstones.end()) {
        return std::nullopt;
    }
    if (auto it = impl_->decompressed.find(k); it != impl_->decompressed.end()) {
        return it->second;
    }

    auto it = impl_->originals.find(k);
    if (it == impl_->originals.end()) {
        return std::nullopt;
    }

    std::size_t out_size = 0;
    void *raw = mz_zip_reader_extract_to_heap(&impl_->reader, it->second.file_index, &out_size, 0);
    if (raw == nullptr) {
        // Decompression failure on a listed entry: surface as absent rather
        // than crashing; the caller reports Missing/Error at its layer.
        return std::nullopt;
    }
    std::vector<std::byte> bytes(out_size);
    if (out_size > 0) {
        std::memcpy(bytes.data(), raw, out_size);
    }
    mz_free(raw);

    ByteBuffer buf{std::move(bytes)};
    impl_->decompressed.emplace(std::move(k), buf);
    return buf;
}

void ZipWorkingSet::writeEntry(std::string_view key, std::vector<std::byte> bytes) {
    std::string k{key};
    impl_->tombstones.erase(k);
    impl_->overrides.insert_or_assign(k, ByteBuffer{std::move(bytes)});
    impl_->dirty = true;
}

void ZipWorkingSet::removeEntry(std::string_view key) {
    std::string k{key};
    const bool had_override = impl_->overrides.erase(k) > 0;
    if (impl_->hasOriginal(k)) {
        impl_->tombstones.insert(k);
        impl_->dirty = true;
    } else if (had_override) {
        impl_->dirty = true;
    }
}

std::vector<ZipDirEntry> ZipWorkingSet::listAtPrefix(std::string_view prefix) const {
    const std::string base = dirBase(prefix);

    // Effective key set: originals not tombstoned, plus overrides. Overrides
    // may add keys not present originally.
    std::unordered_map<std::string, std::uint64_t> effective; // key -> size
    for (const auto &name : impl_->original_order) {
        if (impl_->tombstones.find(name) != impl_->tombstones.end()) {
            continue;
        }
        if (impl_->overrides.find(name) != impl_->overrides.end()) {
            continue; // counted via the override pass below
        }
        effective.emplace(name, impl_->originals.at(name).size);
    }
    for (const auto &[name, buf] : impl_->overrides) {
        effective[name] = buf.size();
    }

    std::vector<ZipDirEntry> out;
    std::unordered_set<std::string> seen_dirs;

    for (const auto &[key, size] : effective) {
        if (!base.empty()) {
            if (key.size() <= base.size() || key.compare(0, base.size(), base) != 0) {
                continue;
            }
        }
        const std::string_view remainder{key.data() + base.size(), key.size() - base.size()};
        if (remainder.empty()) {
            continue;
        }
        const auto slash = remainder.find('/');
        if (slash == std::string_view::npos) {
            out.push_back(ZipDirEntry{std::string{remainder}, key, false, size});
        } else {
            std::string dir_name{remainder.substr(0, slash)};
            std::string dir_key = base + dir_name;
            if (seen_dirs.insert(dir_key).second) {
                out.push_back(ZipDirEntry{std::move(dir_name), std::move(dir_key), true, 0});
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const ZipDirEntry &a, const ZipDirEntry &b) { return a.name < b.name; });
    return out;
}

std::vector<std::byte> ZipWorkingSet::serialize() const {
    mz_zip_archive writer;
    std::memset(&writer, 0, sizeof(writer));
    if (!mz_zip_writer_init_heap(&writer, 0, 0)) {
        throw std::runtime_error("ZipWorkingSet: failed to init ZIP writer");
    }

    // Originals first (stable file-index order), skipping removed/overridden.
    for (const auto &name : impl_->original_order) {
        if (impl_->tombstones.find(name) != impl_->tombstones.end()) {
            continue;
        }
        if (impl_->overrides.find(name) != impl_->overrides.end()) {
            continue;
        }
        const auto &orig = impl_->originals.at(name);
        if (!mz_zip_writer_add_from_zip_reader(&writer, &impl_->reader, orig.file_index)) {
            mz_zip_writer_end(&writer);
            throw std::runtime_error("ZipWorkingSet: failed to copy ZIP entry '" + name + "'");
        }
    }

    // Then overrides (added or replacing an original).
    for (const auto &[name, buf] : impl_->overrides) {
        const auto sp = buf.span();
        if (!mz_zip_writer_add_mem(&writer, name.c_str(), sp.data(), sp.size(),
                                   static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION))) {
            mz_zip_writer_end(&writer);
            throw std::runtime_error("ZipWorkingSet: failed to write ZIP entry '" + name + "'");
        }
    }

    void *out_ptr = nullptr;
    std::size_t out_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&writer, &out_ptr, &out_size)) {
        mz_zip_writer_end(&writer);
        throw std::runtime_error("ZipWorkingSet: failed to finalize ZIP archive");
    }

    std::vector<std::byte> result(out_size);
    if (out_size > 0) {
        std::memcpy(result.data(), out_ptr, out_size);
    }
    mz_zip_writer_end(&writer); // frees the heap buffer behind out_ptr
    return result;
}

bool ZipWorkingSet::dirty() const { return impl_->dirty; }

void ZipWorkingSet::clearDirty() { impl_->dirty = false; }

} // namespace nodehammer::viewer

#include <nodehammer/viewer/archive_export.hpp>

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <nodehammer/detail/file_io.hpp>

#include <cstring>
#include <exception>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

namespace nodehammer::viewer {

namespace {

/// Case-insensitive ".toml" suffix test (matches the BuildSession include gate).
bool isTomlKey(std::string_view key) {
    static constexpr std::string_view kExt = ".toml";
    if (key.size() < kExt.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kExt.size(); ++i) {
        char a = key[key.size() - kExt.size() + i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != kExt[i]) {
            return false;
        }
    }
    return true;
}

/// Resolve `key` and, on Ready, copy its bytes into `ws` under the same key.
/// Returns the resolved bytes for further inspection (include peek), or nullopt
/// when the key did not resolve Ready (appending it to `skipped`).
std::optional<ByteBuffer> takeInto(const ProjectFs &fs, ZipWorkingSet &ws, const std::string &key,
                                   std::vector<std::string> *skipped) {
    auto r = fs.resolve(key);
    if (r.status != ResolveStatus::Ready) {
        if (skipped != nullptr) {
            skipped->push_back(key);
        }
        return std::nullopt;
    }
    const auto sp = r.file.bytes.span();
    ws.writeEntry(key, std::vector<std::byte>(sp.begin(), sp.end()));
    return r.file.bytes;
}

/// Whole-working-set walk for bounded/listable backends: recurse the listing and
/// pull every file leaf into `ws`.
void collectListing(const ProjectFs &fs, ZipWorkingSet &ws, std::vector<std::string> *skipped) {
    // Copy each directory's child keys before recursing: list() spans are only
    // valid until the next generation() bump, and independent-directory spans may
    // share storage in some backends.
    std::deque<std::string> dirs;
    dirs.emplace_back(); // root
    while (!dirs.empty()) {
        const std::string dir = std::move(dirs.front());
        dirs.pop_front();

        std::vector<std::pair<std::string, bool>> children; // (key, is_directory)
        for (const auto &node : fs.list(dir)) {
            children.emplace_back(node.key, node.is_directory);
        }
        for (auto &[key, is_dir] : children) {
            if (is_dir) {
                dirs.push_back(std::move(key));
            } else {
                takeInto(fs, ws, key, skipped);
            }
        }
    }
}

/// Build-closure walk for incomplete backends (filesystem): the root config, its
/// transitive includes, and the geometry blob — nothing else from the tree.
void collectClosure(const ProjectFs &fs, ZipWorkingSet &ws, std::string_view config_key,
                    std::string_view geometry_key, std::vector<std::string> *skipped) {
    std::unordered_set<std::string> seen;
    std::deque<std::string> queue;
    if (!config_key.empty() && seen.insert(std::string{config_key}).second) {
        queue.emplace_back(config_key);
    }
    while (!queue.empty()) {
        const std::string key = std::move(queue.front());
        queue.pop_front();
        auto bytes = takeInto(fs, ws, key, skipped);
        if (!bytes || !isTomlKey(key)) {
            continue;
        }
        for (const auto &rel : ConfigLoader::peekIncludesFromBytes(bytes->span())) {
            auto abs = ConfigLoader::resolveIncludeKey(key, rel);
            if (seen.insert(abs).second) {
                queue.push_back(std::move(abs));
            }
        }
    }
    // Geometry is a single self-contained blob — no include expansion.
    if (!geometry_key.empty() && seen.insert(std::string{geometry_key}).second) {
        takeInto(fs, ws, std::string{geometry_key}, skipped);
    }
}

} // namespace

ZipWorkingSet buildArchiveWorkingSet(const ProjectFs &fs, std::string_view config_key,
                                     std::string_view geometry_key,
                                     std::vector<std::string> *skipped) {
    ZipWorkingSet ws = ZipWorkingSet::create();
    if (fs.listingIsComplete()) {
        collectListing(fs, ws, skipped);
    } else {
        collectClosure(fs, ws, config_key, geometry_key, skipped);
    }
    return ws;
}

#ifndef __EMSCRIPTEN__

bool writeBytesAtomic(const std::filesystem::path &target, std::span<const std::byte> bytes,
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

#endif // __EMSCRIPTEN__

} // namespace nodehammer::viewer

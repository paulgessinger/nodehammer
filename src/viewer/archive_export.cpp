#include <viewer/archive_export.hpp>

#include <config/config_loader.hpp>
#include <lua/lua_config.hpp>
#include <viewer/project_fs.hpp>

#include <algorithm>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <detail/file_io.hpp>

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

/// Case-insensitive suffix test (matches the BuildSession include gate).
bool hasExtCi(std::string_view key, std::string_view ext) {
    if (key.size() < ext.size()) {
        return false;
    }
    for (std::size_t i = 0; i < ext.size(); ++i) {
        char a = key[key.size() - ext.size() + i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != ext[i]) {
            return false;
        }
    }
    return true;
}

bool isTomlKey(std::string_view key) { return hasExtCi(key, ".toml"); }
bool isLuaKey(std::string_view key) { return hasExtCi(key, ".lua"); }

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
        if (!bytes) {
            continue;
        }
        if (isTomlKey(key)) {
            for (const auto &rel : config::ConfigLoader::peekIncludesFromBytes(bytes->span())) {
                auto abs = config::ConfigLoader::resolveIncludeKey(key, rel);
                if (seen.insert(abs).second) {
                    queue.push_back(std::move(abs));
                }
            }
        } else if (isLuaKey(key)) {
            // A script's include set is computed, so there is nothing to peek
            // at: the closure is discovered by running it. The fetcher both
            // serves each fragment and packs it, so what ends up in the archive
            // is exactly what the script reached for — no more (a fragment the
            // script never took would be dead weight) and no less (a missing one
            // would make the archive unopenable).
            //
            // Diagnostics are ignored on purpose. A script that fails to
            // evaluate still had a reason to ask for the files it asked for, and
            // packing them is what lets the user open the archive somewhere else
            // and fix it. Reporting the failure is the build's job, not the
            // exporter's.
            std::unordered_map<std::string, ByteBuffer> pinned;
            auto fetcher = [&](std::string_view k) -> std::optional<std::span<const std::byte>> {
                const std::string sub{k};
                if (const auto it = pinned.find(sub); it != pinned.end()) {
                    return it->second.span();
                }
                auto taken = takeInto(fs, ws, sub, skipped);
                if (!taken) {
                    return std::nullopt;
                }
                auto [ins, _] = pinned.emplace(sub, std::move(*taken));
                return ins->second.span();
            };
            const auto sp = bytes->span();
            (void)lua::evalLuaConfig(
                std::string_view{reinterpret_cast<const char *>(sp.data()), sp.size()}, key,
                fetcher);
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
        detail::file_io::writeFile(tmp, bytes);
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

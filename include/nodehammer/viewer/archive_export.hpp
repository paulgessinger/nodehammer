#pragma once

#include <nodehammer/viewer/zip_working_set.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::viewer {

class ProjectFs;

/// Collect a project's archivable content into a fresh in-memory `ZipWorkingSet`
/// (strategy doc §5.3, step 7). The content set depends on the backend:
///
/// - **`listingIsComplete()` backends** (bag, archive, URL): the *whole working
///   set* — a recursive `list()`/`resolve()` walk. These are bounded, app-owned,
///   user-curated stores, so everything in them is intentional (and the build
///   closure is a subset anyway).
/// - **incomplete backends** (filesystem): the *build closure* only — the root
///   config plus its transitive `include`s (kept as separate files, not merged)
///   plus the geometry blob. Walking the filesystem `list()` would sweep in an
///   unbounded, uncurated directory tree, so we follow the same include walk the
///   `BuildSession` uses instead.
///
/// `config_key` / `geometry_key` are the current build's root keys (ignored for
/// complete backends). Keys that don't resolve `Ready` are appended to `skipped`
/// when it is non-null. The result serializes to a `.zip` via `serialize()`.
ZipWorkingSet buildArchiveWorkingSet(const ProjectFs &fs, std::string_view config_key,
                                     std::string_view geometry_key,
                                     std::vector<std::string> *skipped = nullptr);

#ifndef __EMSCRIPTEN__
/// Durably write `bytes` to `target`: sibling temp file + fsync + rename over the
/// target + parent-dir fsync (POSIX); best-effort remove+rename on Windows. On
/// failure returns false and fills `err`. Shared by `ArchiveProjectFs::saveTo`
/// and the App's save-as path. Native only — web persists by download.
bool writeBytesAtomic(const std::filesystem::path &target, std::span<const std::byte> bytes,
                      std::string &err);
#endif

} // namespace nodehammer::viewer

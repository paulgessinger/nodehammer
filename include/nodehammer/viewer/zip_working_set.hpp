#pragma once

#include <nodehammer/viewer/byte_buffer.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::viewer {

/// One immediate child of a directory synthesized from a ZIP's flat key set.
/// `ZipWorkingSet` has no `ProjectFs` dependency, so it exposes its own listing
/// type; `ArchiveProjectFs` (and the future web bag) map these onto `DirNode`.
struct ZipDirEntry {
    std::string name; // last path component ("common.toml", "detectors")
    std::string key;  // full archive-relative key, forward slashes
    bool is_directory{false};
    std::uint64_t bytes{0}; // uncompressed size for files; 0 for synthesized dirs
};

/// A ZIP-backed read-on-demand store with an in-memory edit overlay (strategy
/// doc §6.5). The whole archive is held in memory and parsed once; original
/// entries decompress lazily on first `read()` and stay cached. Edits
/// (`writeEntry`/`removeEntry`) are recorded as overlays over the read-only
/// archive view — the source bytes are never mutated. `serialize()` streams a
/// fresh ZIP (unchanged entries passed through already-compressed, overrides
/// deflated fresh), which the native archive backend writes to disk and the
/// future web bag persists to IndexedDB.
///
/// Move-only. Keys are forward-slash, archive-relative; callers normalise
/// before handing them in.
class ZipWorkingSet {
  public:
    ~ZipWorkingSet();
    ZipWorkingSet(ZipWorkingSet &&) noexcept;
    ZipWorkingSet &operator=(ZipWorkingSet &&) noexcept;
    ZipWorkingSet(const ZipWorkingSet &) = delete;
    ZipWorkingSet &operator=(const ZipWorkingSet &) = delete;

    /// Parse a ZIP from raw bytes. Throws std::runtime_error if the bytes are
    /// not a readable ZIP central directory.
    static ZipWorkingSet openFromBytes(std::span<const std::byte> bytes);

    /// Read the whole file into memory and parse it. Throws std::runtime_error
    /// if the file can't be read or isn't a readable ZIP.
    static ZipWorkingSet openFromFile(const std::filesystem::path &path);

    /// True if `key` currently resolves to bytes (override present, or an
    /// original that hasn't been removed).
    bool contains(std::string_view key) const;

    /// Bytes for `key`: the override if one was written, else the original
    /// (decompressed and cached on first hit). nullopt if the key was removed
    /// or never existed.
    std::optional<ByteBuffer> read(std::string_view key);

    /// Override (or add) an entry. Stored in memory; the archive is untouched
    /// until serialize(). Clears any pending removal of the same key. Sets dirty.
    void writeEntry(std::string_view key, std::vector<std::byte> bytes);

    /// Remove an entry from the effective view (tombstones an original, or
    /// drops a pending override). Sets dirty.
    void removeEntry(std::string_view key);

    /// Immediate children of `prefix` ("" or "/" for the root), synthesizing
    /// virtual directories from key prefixes. Sorted by name.
    std::vector<ZipDirEntry> listAtPrefix(std::string_view prefix) const;

    /// Serialize the current effective state (originals − removals + overrides)
    /// to a fresh ZIP blob.
    std::vector<std::byte> serialize() const;

    /// True if any writeEntry/removeEntry has been applied since open.
    bool dirty() const;

  private:
    struct Impl;
    explicit ZipWorkingSet(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

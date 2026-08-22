#pragma once

// Turning loose files into a `.nhproj`.
//
// This is `web/stage.cpp`'s `packLooseFiles` promoted out of an anonymous
// namespace, because packing is not a web concern: the same operation backs
// `nodehammer project pack`, the viewer's "Save as archive", and the archive
// `viewer serve` builds on the fly. It lived under `web/` only because that is
// where it was needed first.
//
// The archive it produces is the one `buildArchiveWorkingSet` produces — the
// entry config, its transitive `include` chain, the geometry blob, and the root
// `nodehammer.toml` that names the two entry keys. See
// docs/viewer-project-strategy.md §5.3.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace nodehammer::project {

// Every member carries its own `{}`, including the two that are required. A
// caller naming a subset — `pack({.config = c})`, which is a call this API
// answers, by refusing — is otherwise a `-Wmissing-field-initializers` error
// under GCC, which does not warn for a member that has a default initializer.
struct PackOptions {
    /// Entry config: TOML, or Lua whose include set is discovered by running it.
    std::filesystem::path config{};

    /// Input geometry, in any format this build can import. A `.nhb`/`.nhb.zst`
    /// is embedded verbatim; anything else is imported and re-emitted as
    /// `<stem>.nhb.zst` at the archive root, so the archive always carries the
    /// one format every platform can open without a backend.
    std::filesystem::path geometry{};

    /// Mount point that defines the archive's key space. Empty picks the
    /// deepest directory containing everything that has to be reachable.
    ///
    /// Explicit because keys are the archive's *public surface* — includes
    /// resolve against them — so the choice decides what the archive looks like
    /// from the inside, and a caller who cares should be able to say it.
    std::filesystem::path root{};
};

/// The resolved shape of a pack, for a caller that wants to report it.
struct PackResult {
    std::vector<std::byte> bytes;
    /// The mount point actually used, whether given or derived.
    std::filesystem::path root;
    /// Archive-relative entry keys, as written into `nodehammer.toml`.
    std::string configKey;
    std::string geometryKey;
    /// True when `geometry` was imported rather than embedded verbatim.
    bool imported{false};
};

/// Pack `options` into `.nhproj` bytes. Throws `Error` for anything that would
/// produce an archive that cannot be opened — a missing input, an entry key
/// that escapes the mount, an include that does not resolve.
[[nodiscard]] PackResult pack(const PackOptions &options);

} // namespace nodehammer::project

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

enum class ProjectFsStatus { Idle, Fetching, Ready, Error };

/// Per-file progress for the UI. `bytes_total` stays 0 when the underlying
/// backend can't supply one (local files, picker bytes); the UI should fall
/// back to an indeterminate display in that case.
struct ProjectProgress {
    std::string url;
    std::uint64_t bytes_done{0};
    std::uint64_t bytes_total{0};
    bool done{false};
    bool failed{false};
};

enum class ResolveStatus {
    Ready,   // bytes available; OpenedFile populated
    Pending, // backend is fetching; ask again on a later poll
    Missing, // backend cannot supply this key without user action
    Error,   // backend hit a hard failure for this key
};

/// View into project-owned bytes. The span is valid until the backend
/// next mutates the underlying storage (bag mutation, overlay write,
/// archive remount). Consumers that need to keep the bytes across that
/// boundary must copy.
struct OpenedFile {
    std::string key;
    std::span<const std::byte> bytes;
};

struct ResolveResult {
    ResolveStatus status{ResolveStatus::Missing};
    OpenedFile file;         // valid when status == Ready
    std::string missing_key; // populated when status == Missing
    std::string error;       // populated when status == Error
};

/// Node in a backend's directory snapshot. Backends with a real hierarchy
/// (filesystem, future archive) build a flat owning vector of these on
/// mount/rescan; child spans point into contiguous slices of that storage.
/// Spans are valid until the next `generation()` bump (rescan, archive
/// remount, overlay write).
struct DirNode {
    std::string name; // last path component, e.g. "common.toml"
    std::string key;  // full relative-to-root logical key
    bool is_directory{false};
    std::span<const DirNode> children; // empty for files
    std::uint64_t bytes{0};            // size on disk for files; 0 for directories
};

/// Pluggable virtual filesystem used by the App to load a viewer project.
/// Backends nominate where bytes come from: URL fetches (UrlProjectFs),
/// drag-drop / file-picker bags (BagProjectFs), and (future) archives,
/// watched filesystems, editor overlays.
///
/// Decoration discipline: future stages will wrap an existing project in
/// a decorator (e.g. OverlayProjectFs for the editor, WatchedFilesystemProjectFs
/// for change-on-disk reload). Wrapping is an atomic swap at the App layer:
///
///   app.setProject(std::make_unique<Wrapper>(std::move(impl_->project_)));
///
/// Inner heap objects survive the swap intact, so callbacks holding raw
/// `ProjectFs*` (e.g. emscripten_fetch user_data) keep working. **But**
/// external consumers must always look up the current project via
/// `App::project()` per frame / per event — never cache a `ProjectFs*` as
/// a member, because cached pointers from before an upgrade would bypass
/// the wrapping decorator and silently miss its state.
class ProjectFs {
  public:
    virtual ~ProjectFs() = default;
    ProjectFs() = default;
    ProjectFs(const ProjectFs &) = delete;
    ProjectFs &operator=(const ProjectFs &) = delete;

    /// Cheap to call every frame. Async-driven backends (emscripten_fetch,
    /// sokol drop continuations) update state from their callbacks and
    /// leave this as a no-op; backends that need to advance their own
    /// state machine do their work here.
    virtual void poll() {}

    virtual ProjectFsStatus status() const = 0;
    virtual std::span<const ProjectProgress> progress() const = 0;
    virtual const std::string &errorMessage() const = 0;

    /// Short human-readable backend identifier for debug UI ("bag",
    /// "url", future "archive"/"watched"/"overlay"). No leading caps —
    /// the UI capitalises on render if it cares.
    virtual std::string_view name() const = 0;

    /// Soft warnings that don't fail the build (e.g. bag's "replaced
    /// foo.toml" note when a same-basename drop overwrites an existing
    /// entry). Empty by default; backends that emit warnings override.
    virtual std::span<const std::string> warnings() const { return {}; }

    /// User-gesture file ingestion. Default no-ops so URL-style backends
    /// don't have to override; BagProjectFs implements both. The platform
    /// layer pushes through these without needing to know the concrete
    /// backend, which is what avoids a dynamic_cast at every call site.
    virtual void addPath(const std::filesystem::path & /*path*/) {}
    virtual void addBytes(std::string_view /*filename*/, std::span<const std::byte> /*bytes*/) {}

    /// Look up the bytes for a logical key. Backends that have everything
    /// in memory (the bag, an archive) return Ready or Missing
    /// synchronously; backends that fetch lazily (the URL backend) return
    /// Pending while the bytes are in flight and Ready once they land.
    /// The returned span is valid until the next backend mutation.
    virtual ResolveResult resolve(std::string_view key) const {
        return ResolveResult{ResolveStatus::Missing, {}, std::string{key}, {}};
    }

    /// Bumps on every state change that could affect a build (file added,
    /// replaced, removed, overlay write). Consumers that cache build
    /// results (the BuildSession's gate) read this in addition to the
    /// root keys, so an include-only update — e.g. dropping a missing
    /// `common.toml` after a build failed for missing-include — still
    /// fires a fresh walk even though the root paths didn't change.
    virtual std::uint64_t generation() const { return 0; }

    /// Optional directory-listing capability. Backends with a real
    /// hierarchy (FilesystemProjectFs, future archive) override; the bag
    /// and URL backends return an empty span and the App's tree panel
    /// falls back to its flat progress UI. The returned span and the
    /// children spans inside it are valid until the next `generation()`
    /// bump. `dir` is the empty string (or "/") for the root.
    virtual std::span<const DirNode> list(std::string_view /*dir*/ = {}) const { return {}; }

    /// Force the backend to drop any cached state and rebuild its
    /// snapshot from the source of truth. Default: no-op. The filesystem
    /// backend re-walks its mount root and invalidates its byte cache;
    /// in-memory backends (bag, URL) have nothing to refresh. Bumps
    /// `generation()` if the backend has anything cached.
    virtual void rescan() {}
};

} // namespace nodehammer::viewer

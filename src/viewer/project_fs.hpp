#pragma once

#include <viewer/byte_buffer.hpp>
#include <viewer/log_sink.hpp>

#include <cstddef>
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

// No asynchronous outcome, deliberately. Every backend owns a complete working
// set by the time it exists: the web viewer fetches a whole `.nhproj` and only
// *then* constructs an ArchiveProjectFs over it, and the IndexedDB restore does
// the same. Loading is modelled as a different project, never as a project that
// is still loading — which is what lets resolution be a plain function call.
//
// There used to be a `Pending` here, for a URL backend that fetched entries on
// demand. It went away with the move to ZIP working sets, and the machinery that
// resumed a half-finished walk across frames went with it. Bringing per-entry
// lazy loading back would need this again — and would deserve a design aimed at
// that, rather than a fossil kept warm in case.
enum class ResolveStatus {
    Ready,   // bytes available; OpenedFile populated
    Missing, // backend cannot supply this key without user action
    Error,   // backend hit a hard failure for this key
};

/// Refcounted handle to project-owned bytes. The buffer pins the bytes
/// by refcount, so consumers may hold an `OpenedFile` (or just its
/// `ByteBuffer`) across backend mutations and the underlying storage
/// stays alive until the last handle releases it. Cache-backed backends
/// (bag, URL, future ZIP) share storage via refcount; storage-backed
/// backends (filesystem, future native bag) build a fresh `ByteBuffer`
/// per call.
struct OpenedFile {
    std::string key;
    ByteBuffer bytes;
};

struct ResolveResult {
    ResolveStatus status{ResolveStatus::Missing};
    OpenedFile file;         // valid when status == Ready
    std::string missing_key; // populated when status == Missing
    std::string error;       // populated when status == Error
};

/// Node in a backend's directory listing. Backends expose hierarchy lazily:
/// children of a directory are obtained by calling `list(node.key)`, not
/// embedded here. Spans returned by `list()` are valid until the next
/// `generation()` bump.
struct DirNode {
    std::string name; // last path component, e.g. "common.toml"
    std::string key;  // full relative-to-root logical key
    bool is_directory{false};
    std::uint64_t bytes{0}; // size on disk for files; 0 for directories
};

struct ProjectDropDecision {
    enum class Kind {
        Accept,  // Apply immediately via addPath/addBytes.
        Confirm, // App should ask before applying via addPath/addBytes.
        Reject,  // App should present the message and leave the project unchanged.
    };

    Kind kind{Kind::Reject};
    std::string title;
    std::string message;
    std::string confirm_label{"OK"};
    std::string cancel_label{"Cancel"};
};

/// Pluggable virtual filesystem used by the App to load a viewer project.
/// Backends nominate where bytes come from: on-disk folders
/// (FilesystemProjectFs, WatchedFilesystemProjectFs), the native drop/pick bag
/// (NativeBagProjectFs), and the ZIP working set that backs archive mode + the
/// web app/viewer project (ArchiveProjectFs).
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
class ProjectFs : public LogSinkHolder {
  public:
    ~ProjectFs() override = default;
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

    /// Short human-readable backend identifier for debug UI ("bag",
    /// "url", future "archive"/"watched"/"overlay"). No leading caps —
    /// the UI capitalises on render if it cares.
    virtual std::string_view name() const = 0;

    /// True when `list()` enumerates the entire backing store — i.e. walking
    /// `list("")` recursively yields every file the backend holds. In-memory /
    /// app-owned backends (bag, archive, URL manifest) are complete; the
    /// filesystem backend is not (its root is an unbounded real directory tree
    /// the user did not curate). Consumers that need the full content set (e.g.
    /// "export as archive") walk the listing for complete backends and fall back
    /// to the build closure otherwise.
    virtual bool listingIsComplete() const { return false; }

    /// Soft warnings that don't fail the build (e.g. bag's "replaced
    /// foo.toml" note when a same-basename drop overwrites an existing
    /// entry). Empty by default; backends that emit warnings override.
    virtual std::span<const std::string> warnings() const { return {}; }

    /// User-gesture file ingestion policy. The App asks for a decision first
    /// so backend-specific behavior (bag replacement, filesystem read-only,
    /// URL read-only, future overlays) stays with the ProjectFs, while the
    /// App owns any modal UI needed for Confirm/Reject decisions.
    virtual ProjectDropDecision planAddPath(const std::filesystem::path & /*path*/) const {
        return ProjectDropDecision{
            ProjectDropDecision::Kind::Reject,
            "Cannot add file",
            "The current project backend does not accept dropped local files.",
            "OK",
            {},
        };
    }
    virtual ProjectDropDecision planAddBytes(std::string_view /*filename*/,
                                             std::span<const std::byte> /*bytes*/) const {
        return ProjectDropDecision{
            ProjectDropDecision::Kind::Reject,
            "Cannot add file",
            "The current project backend does not accept uploaded file bytes.",
            "OK",
            {},
        };
    }

    /// Commit a user-approved file ingestion. Default no-ops preserve the old
    /// backend contract for direct callers; App code should use planAdd* first.
    virtual void addPath(const std::filesystem::path & /*path*/) {}
    virtual void addBytes(std::string_view /*filename*/, std::span<const std::byte> /*bytes*/) {}

    /// File-removal policy, mirroring planAdd*: the App asks first so the
    /// backend owns whether (and how) a key may be removed, while the App owns
    /// the confirmation modal. Editable in-memory backends (the archive) return
    /// Confirm; read-only / filesystem-mounted backends keep the default Reject.
    virtual ProjectDropDecision planRemove(std::string_view /*key*/) const {
        return ProjectDropDecision{
            ProjectDropDecision::Kind::Reject,
            "Cannot remove file",
            "The current project backend does not support removing files.",
            "OK",
            {},
        };
    }

    /// Commit a user-approved removal. Default no-op; App code should use
    /// planRemove first. Bumps generation() on a real removal.
    virtual void removeKey(std::string_view /*key*/) {}

    /// Relocation policy for an in-app move (e.g. dragging a file onto a folder
    /// in the tree). Mirrors planAdd*/planRemove: `to_key` is the full target
    /// key. Editable backends confirm when `to_key` already exists and accept
    /// otherwise; read-only backends keep the default Reject.
    virtual ProjectDropDecision planMove(std::string_view /*from_key*/,
                                         std::string_view /*to_key*/) const {
        return ProjectDropDecision{
            ProjectDropDecision::Kind::Reject,
            "Cannot move file",
            "The current project backend does not support moving files.",
            "OK",
            {},
        };
    }

    /// Commit a user-approved move (re-key `from_key` to `to_key`, preserving
    /// bytes). Default no-op; App code should use planMove first. Bumps
    /// generation() on a real move.
    virtual void moveKey(std::string_view /*from_key*/, std::string_view /*to_key*/) {}

    /// Look up the bytes for a logical key, synchronously. Every backend has
    /// its working set in memory, so this answers Ready, Missing or Error and
    /// never asks to be called again. The returned `ByteBuffer` holds its bytes
    /// by refcount; consumers may keep it across backend mutations.
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

    /// Lazy per-directory listing. `list("")` (or `list("/")`) returns the
    /// immediate children of the project root; `list(dir_key)` returns the
    /// immediate children of `dir_key`, where `dir_key` matches the `key`
    /// of some prior `DirNode` with `is_directory == true`. Other inputs
    /// return an empty span. The returned span is valid until the next
    /// `generation()` bump; backends may keep per-directory caches across
    /// calls within a single generation. Spans across different
    /// directories are independent and may live in different storage.
    virtual std::span<const DirNode> list(std::string_view /*dir*/ = {}) const { return {}; }

    /// Force the backend to drop any cached state and rebuild its
    /// snapshot from the source of truth. Default: no-op. The filesystem
    /// backend re-walks its mount root and invalidates its byte cache;
    /// in-memory backends (bag, URL) have nothing to refresh. Bumps
    /// `generation()` if the backend has anything cached.
    virtual void rescan() {}
};

} // namespace nodehammer::viewer

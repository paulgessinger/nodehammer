#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

/// Stage 2 placeholder: returned once `resolve(key)` is wired through the
/// include walker. Stage 1 implementations stub out resolve().
struct ResolvedFile {
    std::filesystem::path path;
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

    /// Build trigger: filesystem paths the App passes to SceneBuildJob.
    /// Valid once status() == Ready. May be empty until then. Stage 2 will
    /// replace these with byte-span resolution through `resolve(key)`.
    virtual const std::filesystem::path &rootConfigPath() const = 0;
    virtual const std::filesystem::path &rootInputPath() const = 0;

    /// Human-readable hints about files the project is still expecting
    /// (e.g. "a .toml config", "a geometry file"). Empty when satisfied.
    virtual std::span<const std::string> waitingFor() const { return {}; }

    /// Soft warnings that don't fail the build (e.g. "replaced foo.toml
    /// with /tmp/.../foo.toml"). Surfaced in the UI alongside waitingFor.
    virtual std::span<const std::string> warnings() const { return {}; }

    /// The most recent file the backend was offered but didn't recognise,
    /// or empty. Drives the "don't know what to do with X" UI hint.
    virtual const std::string &unrecognised() const {
        static const std::string empty;
        return empty;
    }

    /// User-gesture file ingestion. Default no-ops so URL-style backends
    /// don't have to override; BagProjectFs implements both. The platform
    /// layer pushes through these without needing to know the concrete
    /// backend, which is what avoids a dynamic_cast at every call site.
    virtual void addPath(const std::filesystem::path & /*path*/) {}
    virtual void addBytes(std::string_view /*filename*/, std::span<const std::byte> /*bytes*/) {}

    /// Stage 2 will plumb this through the include walker. Stage 1 stub
    /// returns nullopt. Defined here so Stage 2 lands as a pure addition.
    virtual std::optional<ResolvedFile> resolve(std::string_view /*key*/) const {
        return std::nullopt;
    }
};

} // namespace nodehammer::viewer

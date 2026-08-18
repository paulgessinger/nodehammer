#pragma once

#include <config/config_loader.hpp>
#include <ir/semantic/importer.hpp>
#include <viewer/log_sink.hpp>
#include <viewer/project_fs.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

/// Phases of the resolve → parse → import pipeline that turns project
/// bytes into the inputs the scene build job consumes. The session does
/// *not* own the scene build job itself; once `phase()` reaches
/// `ResolvedReady`, the App moves the parsed config + imported scene
/// into `SceneBuildJob::start(...)`. After consumption the session
/// stays in `Consumed` until something invalidates it (a project
/// generation bump, or an explicit `invalidate()` call from the App).
enum class BuildPhase {
    Idle,           ///< no root keys set yet (or session was invalidated and root keys cleared)
    Stale,          ///< derived state no longer matches the project; the next refresh re-derives
    WaitingForUser, ///< at least one key is Missing — surface in UI; session holds
    ResolvedReady,  ///< all bytes resolved and parsed; consume via `takeInputs()`
    Consumed,       ///< inputs handed to the App; await invalidation
    Error,          ///< hard failure (parse error, fetch error, etc.); message went to the sink
};

/// Inputs to `SceneBuildJob::start` produced by a successful walk +
/// parse + import. Moves out of the session via `takeInputs()`.
struct BuildSessionInputs {
    config::ConfigResult config;
    ir::ImportResult import;
    std::string config_key;
    std::string geometry_key;
    /// Hash over the full set of resolved input bytes (root config + every
    /// transitive include + geometry). The parse → import → tessellate pipeline
    /// is a deterministic function of these bytes, so a consumer can skip the
    /// expensive rebuild when this matches the last one it built (e.g. after a
    /// backend swap that resolves byte-identical content, like promoting a
    /// project to an archive). Zero only for an empty input set.
    std::uint64_t input_hash{0};
};

/// Derives a build's inputs from a `ProjectFs`. On each `refresh()`, if
/// the project hasn't bumped its `generation()` since the last one the
/// session is a no-op; otherwise it re-derives in one
/// pass: resolve the two root keys, parse the config — its includes
/// resolved through a fetcher that reads the project directly — and
/// import the geometry, then transition to `ResolvedReady`.
///
/// There is no separate resolution phase. There used to be, because a
/// URL backend resolved keys asynchronously and a walk had to be
/// resumable across frames; every backend is synchronous now, so
/// discovering an include and reading it are the same act. That is also
/// the only arrangement a Lua config admits: its include set is computed
/// rather than declared, so there is nothing to walk ahead of running it.
///
/// What the fetcher could not supply is what distinguishes
/// `WaitingForUser` from `Error`, and it is asked rather than inferred
/// from diagnostics: `ProjectFs` already separates "the project does not
/// have this" from "the backend broke", and only the first is something
/// the user can fix by adding a file.
class BuildSession : public LogSinkHolder {
  public:
    BuildSession();
    ~BuildSession() override;

    BuildSession(const BuildSession &) = delete;
    BuildSession &operator=(const BuildSession &) = delete;

    /// Set or replace the root keys. Resets phase to `Idle` if either
    /// key is empty, or `Stale` if both are set. Existing in-flight
    /// state is discarded — if a previous build was already
    /// `ResolvedReady`/`Consumed` and the new keys differ, the previous
    /// inputs are dropped.
    void setRootKeys(std::string config_key, std::string geometry_key);

    /// Force a re-derive on the next refresh, regardless of project
    /// generation. Used after a project swap (close project, archive
    /// restore) so cached state from the prior project is dropped.
    void invalidate();

    /// Bring the derived state in line with the project, re-deriving it
    /// from scratch if the project has moved since last time. Cheap to
    /// call every frame: when the generation hasn't moved and the session
    /// is settled, this returns immediately.
    ///
    /// Not called `poll`, because it polls nothing of its own — the work
    /// it does is finished by the time it returns. `ProjectFs::poll()` is
    /// the real poll in this pair: that one drains change notifications a
    /// watcher raised on a background thread. The project is taken by
    /// reference and read live, never cached (the decoration discipline
    /// rule).
    void refresh(ProjectFs &project);

    BuildPhase phase() const;

    /// Keys the session is currently unable to resolve (Missing or
    /// Error). Empty during normal operation; populated when
    /// `phase() == WaitingForUser` or `Error`.
    std::span<const std::string> missing() const;

    /// Move the resolved inputs out of the session. Only valid when
    /// `phase() == ResolvedReady`; transitions to `Consumed`. Returns
    /// nullptr in any other phase (caller should check `phase()` first).
    [[nodiscard]] std::unique_ptr<BuildSessionInputs> takeInputs();

    /// Current root keys (informational; for the App's UI / build-trigger
    /// gate identity).
    const std::string &rootConfigKey() const;
    const std::string &rootGeometryKey() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

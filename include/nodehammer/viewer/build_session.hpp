#pragma once

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/ir/semantic/importer.hpp>
#include <nodehammer/log_sink.hpp>
#include <nodehammer/viewer/project_fs.hpp>

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
    Walking,        ///< resolving the include graph; some keys may be Pending
    WaitingForUser, ///< at least one key is Missing — surface in UI; session holds
    ResolvedReady,  ///< all bytes resolved and parsed; consume via `takeInputs()`
    Consumed,       ///< inputs handed to the App; await invalidation
    Error,          ///< hard failure (parse error, fetch error, etc.); message went to the sink
};

/// Inputs to `SceneBuildJob::start` produced by a successful walk +
/// parse + import. Moves out of the session via `takeInputs()`.
struct BuildSessionInputs {
    ConfigResult config;
    ImportResult import;
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

/// Drives the include-graph walk against a `ProjectFs`. On each
/// `poll()`, if the project hasn't bumped its `generation()` since the
/// last poll the session is a no-op; otherwise it walks: resolves the
/// root config key, peeks its includes, resolves each (recursively),
/// then resolves the input key. Once every reachable key is `Ready`,
/// the session parses the config (via `ConfigLoader::parseAndMerge`)
/// and imports the geometry (via `FlatBufferImporter::importFromBytes`),
/// and transitions to `ResolvedReady`.
///
/// Backend agnosticism: the session calls `project->resolve(key)` and
/// honours all four `ResolveStatus` outcomes. `Pending` keeps the
/// session in `Walking`; the next poll re-tries. This is what lets the
/// URL backend's lazy fetches work without the session owning any
/// fetch logic itself.
class BuildSession : public LogSinkHolder {
  public:
    BuildSession();
    ~BuildSession() override;

    BuildSession(const BuildSession &) = delete;
    BuildSession &operator=(const BuildSession &) = delete;

    /// Set or replace the root keys. Resets phase to `Idle` if either
    /// key is empty, or `Walking` if both are set. Existing in-flight
    /// state is discarded — if a previous build was already
    /// `ResolvedReady`/`Consumed` and the new keys differ, the previous
    /// inputs are dropped.
    void setRootKeys(std::string config_key, std::string geometry_key);

    /// Force a re-walk on the next poll, regardless of project
    /// generation. Used after a project swap (close project, URL → bag
    /// graduation) so cached state from the prior project is dropped.
    void invalidate();

    /// Drive the state machine. Reads the project via the provided
    /// `project` pointer. Cheap to call every frame: when the project's
    /// generation hasn't moved and the session is settled, this is a
    /// no-op. The pointer is read live (matches the decoration
    /// discipline rule that consumers don't cache `ProjectFs*`).
    void poll(ProjectFs *project);

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

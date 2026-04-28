#pragma once

#include "ibl.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nodehammer::viewer {

/// Bumped whenever the bake algorithm or output layout changes. A mismatch
/// invalidates any on-disk / IDB cache entry without manual cleanup.
///
/// To force a cache rebuild for all users:
///   1. Increment this constant by one.
///   2. That's it — no need to touch the IDB key or on-disk filename. The
///      version field is checked inside `deserializeIblCache`; old caches
///      fail verification and `IblCacheLoad` reports a miss, which causes
///      the viewer to re-bake and overwrite the stale entry on save.
///
/// Bump triggers (non-exhaustive):
///   - Any change to the math in `bakeIbl()` / `IblBakeJob` (sample counts,
///     sky function, importance sampling, tone-mapping of the output).
///   - Any change to the `IblBakeData` constants (`kIrradianceSize`,
///     `kPrefilterSize`, `kPrefilterMips`, `kBrdfLutSize`) or the byte layout
///     of any buffer (e.g. switching RGBA8 → RGBA16F).
///   - Any change to the FlatBuffers schema in `schemas/ibl_cache.fbs` that
///     isn't backwards-compatible (renaming/reordering fields, changing
///     types). Pure additions of optional fields don't strictly require a
///     bump but bumping is the safe default.
inline constexpr uint32_t kIblCacheVersion = 1;

/// Serialize a baked IBL dataset to a self-contained FlatBuffer (with the
/// "NHIB" file_identifier). Round-trip via `deserializeIblCache`.
[[nodiscard]] std::vector<std::byte> serializeIblCache(const IblBakeData &data);

/// Verify and decode a buffer previously produced by `serializeIblCache`.
/// Returns nullopt if the buffer fails verification, has the wrong version,
/// or any byte slice has an unexpected size for the current `IblBakeData`
/// layout.
[[nodiscard]] std::optional<IblBakeData> deserializeIblCache(std::span<const std::byte> buf);

/// Async helper: try to load a previously baked IBL from the platform
/// cache. On native this resolves synchronously inside `start()`; on web
/// it kicks off an `emscripten_idb_async_load` and completes during a
/// later `poll()`.
class IblCacheLoad {
  public:
    IblCacheLoad() = default;
    ~IblCacheLoad();
    IblCacheLoad(const IblCacheLoad &) = delete;
    IblCacheLoad &operator=(const IblCacheLoad &) = delete;

    /// Begin the load. Idempotent — second call is a no-op.
    void start();

    /// Returns true once a hit/miss decision is available. Until then the
    /// caller should keep polling each frame (web only).
    [[nodiscard]] bool poll();

    /// Move out the loaded bake, if any. Valid after `poll()` returns true;
    /// returns nullopt on miss or any verification failure.
    [[nodiscard]] std::optional<IblBakeData> take();

  private:
    enum class State : uint8_t { Idle, Pending, Hit, Miss };
    State state_{State::Idle};
    std::optional<IblBakeData> data_;

#ifdef __EMSCRIPTEN__
    // Static C-style callbacks dispatch back to the instance via user_data.
    static void onLoad(void *user_data, void *bytes, int size);
    static void onError(void *user_data);
#endif
};

/// Persist the bake to the platform cache. Best-effort: errors are logged
/// but never thrown — a failed save just means the next launch re-bakes.
/// On web this dispatches `emscripten_idb_async_store` and returns
/// immediately; the store completes asynchronously on the JS side.
void saveIblCache(const IblBakeData &data);

/// Delete any cached IBL bake. Best-effort: a missing entry is not an
/// error. Native: removes the cache file. Web: dispatches
/// `emscripten_idb_async_delete`. The currently-installed GPU IBL is not
/// affected — only the on-disk / IDB copy is cleared. Useful as a
/// developer escape hatch if the bake is suspected to be stale and the
/// version bump in `kIblCacheVersion` was forgotten.
void clearIblCache();

} // namespace nodehammer::viewer

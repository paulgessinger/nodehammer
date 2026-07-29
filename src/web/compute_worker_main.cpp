// Headless compute module for the viewer's Web Worker.
//
// This is the second wasm instance in the "compute off the main thread without
// SharedArrayBuffer" design (see docs / the web-worker-compute branch). It links
// only the pipeline core — no sokol/viewer/GL — and exposes a single C entry the
// worker JS calls per build. Parallelism comes from this running in a separate
// Worker thread with its own heap; data crosses the boundary as bytes
// (semantic .nhb in, render .nhr out), so no pthreads / cross-origin isolation
// is required.
//
// Lifecycle:
//   1. worker JS instantiates the MODULARIZEd module once (runtime kept alive,
//      EXIT_RUNTIME=0).
//   2. For each build it copies the semantic-scene bytes + flat config TOML into
//      the heap and calls `nh_compute_build`, which deserializes, runs
//      prep -> (optional) wedge cut -> tessellation to completion, and returns
//      malloc'd NHR8 render bytes the JS transfers back to the main thread.
//   3. Progress / errors are pushed to the main thread directly via EM_JS
//      postMessage so the existing viewer progress UI keeps working.
//
// The deserialized pristine scene + parsed config are cached by an opaque epoch
// so re-aiming the wedge cut (same scene, new angle) doesn't re-deserialize:
// the caller passes the bytes only when the epoch changes.

#include <config/config_loader.hpp>
#include <ir/fb/render/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <scene_build.hpp>
#include <tessellation/build_pipeline.hpp>
#include <tessellation/wedge_cut.hpp>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace {

using namespace nodehammer;

// Phase codes posted to the main thread; the JS side maps them to labels.
// Ordered to mirror BuildPipeline::Phase (Preparing/Cutting/Tessellating/Finalizing).
enum Phase : int { kPreparing = 1, kCutting = 2, kTessellating = 3, kFinalizing = 4 };

// The wire codes cross a JS boundary so they can't just be the enum, but they
// must stay bound to it: a future reorder of BuildPipeline::Phase then becomes a
// compile error here instead of a silent UI mislabel (invariant #2).
static_assert(kPreparing == static_cast<int>(BuildPipeline::Phase::Preparing));
static_assert(kCutting == static_cast<int>(BuildPipeline::Phase::Cutting));
static_assert(kTessellating == static_cast<int>(BuildPipeline::Phase::Tessellating));
static_assert(kFinalizing == static_cast<int>(BuildPipeline::Phase::Finalizing));

// Off-thread, so the slice budget only governs how often we emit progress (not
// frame responsiveness). ~50 ms keeps postMessage traffic light while still
// giving several updates across a multi-second ODD build.
constexpr std::uint64_t kSliceNs = 50'000'000;

// Cap on progress messages posted per phase. The advance() loop may iterate far
// more often than this (a coarsened worker clock can yield very fine slices), so
// emit only when the processed count crosses the next 1/kProgressUpdates step.
// This keeps the postMessage channel — and the main thread draining it — from
// being buried under thousands of updates on a large scene.
constexpr std::size_t kProgressUpdates = 100;

// clang-format off
// NOTE: the postMessage object keys are quoted on purpose. This module is
// compiled with Closure (--closure=1) in the Release build, which renames
// unquoted object-literal keys; the receiving viewer bundle is a *separate*
// Closure compilation (and compute_worker.js isn't Closure'd at all), so an
// unquoted contract renames inconsistently across the worker boundary and the
// messages stop matching. Quoting pins the wire names. Keep these in sync with
// compute_worker.js and scene_build_job_web_worker.cpp.
EM_JS(void, nh_compute_emit_progress, (int phase, double processed, double total), {
    if (typeof postMessage === 'function') {
        postMessage({ 'nh': 'progress', 'phase': phase, 'processed': processed, 'total': total });
    }
});

EM_JS(void, nh_compute_emit_error, (const char *msg), {
    if (typeof postMessage === 'function') {
        postMessage({ 'nh': 'error', 'message': UTF8ToString(msg) });
    }
});
// clang-format on

void reportError(const DiagnosticList &diags, const char *fallback) {
    std::string msg;
    for (const auto &d : diags.items()) {
        if (d.severity >= DiagnosticSeverity::Error) {
            if (!msg.empty()) {
                msg += "; ";
            }
            msg += d.code;
            msg += ": ";
            msg += d.message;
        }
    }
    nh_compute_emit_error(msg.empty() ? fallback : msg.c_str());
}

// Cached pristine inputs, reused across builds with the same epoch so a wedge
// re-aim doesn't pay the deserialize/parse cost again.
struct ComputeCache {
    bool valid{false};
    std::uint32_t epoch{0};
    SemanticScene scene;
    NHConfig config;
};

ComputeCache &cache() {
    static ComputeCache c;
    return c;
}

} // namespace

extern "C" {

// Build a render scene from a (cached or freshly supplied) semantic scene.
//
//   epoch        opaque token identifying the pristine scene. When it matches
//                the cache and scene_bytes is null, the cached scene is reused.
//   scene_bytes  uncompressed semantic FlatBuffer (.nhb) bytes, or null to use
//                the cache. scene_len is its length.
//   config_toml  flattened config TOML (includes already inlined), or null to
//                reuse the cached config. Required whenever scene_bytes is given.
//   has_wedge    non-zero applies an azimuthal wedge cut before tessellation.
//   out_len      receives the length of the returned buffer (0 on failure).
//
// Returns a malloc'd buffer of NHR8 render bytes (caller frees), or null on
// failure (an error message has been posted via EM_JS).
EMSCRIPTEN_KEEPALIVE
std::uint8_t *nh_compute_build(std::uint32_t epoch, const std::uint8_t *scene_bytes,
                               std::size_t scene_len, const char *config_toml, int has_wedge,
                               double wedge_start_deg, double wedge_end_deg, double wedge_margin,
                               std::uint32_t *out_len) {
    if (out_len != nullptr) {
        *out_len = 0;
    }

    ComputeCache &c = cache();

    // (Re)seed the cache when new scene bytes arrive; otherwise require a hit.
    if (scene_bytes != nullptr && scene_len > 0) {
        auto imported = FlatBufferImporter::importFromBytes(
            "scene.nhb", std::as_bytes(std::span{scene_bytes, scene_len}));
        if (imported.diags.hasErrors()) {
            reportError(imported.diags, "compute: failed to deserialize semantic scene");
            return nullptr;
        }
        auto loaded = ConfigLoader::loadFromString(config_toml != nullptr ? config_toml : "",
                                                   "<worker-config>");
        if (loaded.diags.hasErrors()) {
            reportError(loaded.diags, "compute: failed to parse config");
            return nullptr;
        }
        c.scene = std::move(imported.scene);
        c.config = std::move(loaded.config);
        c.epoch = epoch;
        c.valid = true;
    } else if (!c.valid || c.epoch != epoch) {
        nh_compute_emit_error("compute: no cached scene for requested epoch");
        return nullptr;
    }

    // Drive the shared core BuildPipeline. Prep copies the cached scene + config
    // internally and never mutates the pointee, so the cache stays pristine for
    // the next wedge re-aim; the wedge is deferred to a WedgeCutJob for progress,
    // exactly like the native/cooperative backends. The inputs are handed in as
    // *non-owning* aliasing shared_ptrs over the cache — the pipeline never takes
    // ownership of the cached scene/config (invariant #5).
    auto cfg = std::shared_ptr<const NHConfig>(std::shared_ptr<void>{}, &c.config);
    auto scn = std::shared_ptr<const SemanticScene>(std::shared_ptr<void>{}, &c.scene);

    nh_compute_emit_progress(kPreparing, 0, 0);
    BuildPipeline pipe;
    pipe.start(cfg, scn,
               has_wedge != 0 ? std::optional<WedgeCutParams>{WedgeCutParams{
                                    wedge_start_deg, wedge_end_deg, wedge_margin}}
                              : std::nullopt);
    // Throttle progress emits to ~kProgressUpdates per phase: advance() may
    // yield far more often than that on a coarsened worker clock (see
    // kClockCheckStride in tessellation_pass.cpp/wedge_cut.cpp), and a
    // postMessage per yield would bury the main thread on a large scene.
    std::size_t next_cut = 0;
    std::size_t next_tess = 0;
    while (!pipe.advance(kSliceNs)) {
        switch (pipe.phase()) {
        case BuildPipeline::Phase::Cutting: {
            const std::size_t processed = pipe.wedgeCutProcessed();
            const std::size_t total = pipe.wedgeCutTotal();
            if (processed >= next_cut) {
                nh_compute_emit_progress(kCutting, static_cast<double>(processed),
                                         static_cast<double>(total));
                next_cut = processed + std::max<std::size_t>(1, total / kProgressUpdates);
            }
            break;
        }
        case BuildPipeline::Phase::Tessellating: {
            const std::size_t processed = pipe.tessellationProcessed();
            const std::size_t total = pipe.tessellationTotal();
            if (processed >= next_tess) {
                nh_compute_emit_progress(kTessellating, static_cast<double>(processed),
                                         static_cast<double>(total));
                next_tess = processed + std::max<std::size_t>(1, total / kProgressUpdates);
            }
            break;
        }
        case BuildPipeline::Phase::Finalizing:
            nh_compute_emit_progress(kFinalizing, 0, 0);
            break;
        default:
            nh_compute_emit_progress(kPreparing, 0, 0);
            break;
        }
    }
    SceneBuildResult result = pipe.take();
    if (result.scene == nullptr) {
        reportError(result.diags, "compute: build failed");
        return nullptr;
    }

    nh_compute_emit_progress(kFinalizing, 0, 0);
    std::vector<std::byte> bytes = renderSceneToBytes(*result.scene);
    auto *buffer = static_cast<std::uint8_t *>(std::malloc(bytes.size()));
    if (buffer == nullptr) {
        nh_compute_emit_error("compute: out of memory packaging render scene");
        return nullptr;
    }
    std::memcpy(buffer, bytes.data(), bytes.size());
    if (out_len != nullptr) {
        *out_len = static_cast<std::uint32_t>(bytes.size());
    }
    return buffer;
}

} // extern "C"

// Empty main(): the module is instantiated and kept alive (EXIT_RUNTIME=0); the
// worker drives it later via nh_compute_build.
int main(int /*argc*/, char ** /*argv*/) { return 0; }

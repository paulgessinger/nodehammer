#pragma once

#include <string_view>

// NH diagnostic codes.
//
// A code names *what went wrong*. It does not name a channel — that belongs to
// the call, since a failure is fatal only when the call cannot deliver what it
// promised (docs/error-model.md). The prefix says how far that goes:
//
//   kFatal…   no call can observe this non-fatally. Always thrown as `Error`,
//             never present in a `DiagnosticList`.
//   kErr…     reported at Error severity — the result exists and part of it is
//             missing or wrong. A `kErr…` code may *also* be thrown, by a call
//             that promised the very result the error makes partial: the config
//             family below is reported by `Config::check` and thrown by
//             `Config::read`.
//   kWarn…    the result is complete; something was assumed or substituted.
//   kInfo…    the result is exactly what was asked for; this is worth recording.
//   kDebug…   trace.
//
// Ranges:
//   NH0001–NH0099  config
//   NH0100–NH0199  import (general)
//   NH0200–NH0299  scene operations (dedup)
//   NH0300–NH0399  TGeo importer
//   NH0400–NH0499  selection engine
//   NH0500–NH0599  tessellation
//   NH0600–NH0699  export
//   NH0700–NH0799  compute worker (web)
//   NH0800–NH0899  public API boundary
//   NH0900–NH0999  CLI usage
//   NH1000–NH1099  web viewer runtime

namespace nodehammer::codes {

// ── Config ────────────────────────────────────────────────────────────────────
//
// The whole family is dual-channel. `ConfigLoader` collects rather than stops —
// reporting every problem in a document is the point — so these reach a caller
// as diagnostics through `Config::check`, and as a thrown `Error` through
// `Config::read`, which promised a config rather than a report.
inline constexpr std::string_view kErrConfigParse = "NH0001";
inline constexpr std::string_view kErrUndefinedMaterialRef = "NH0002";
inline constexpr std::string_view kErrNegativeTolerance = "NH0003";
inline constexpr std::string_view kErrMissingOutputPath = "NH0004";
inline constexpr std::string_view kWarnConfigUnknownKey = "NH0005";
inline constexpr std::string_view kWarnConfigEmptyScope = "NH0006";
inline constexpr std::string_view kWarnConfigUnconditionalMaterialRule = "NH0007";
// Warning: drop_coincident_faces is enabled somewhere but merge_descendants is
// never enabled — drop_coincident_faces is a no-op without it (it operates on
// the merged group), so the option will silently do nothing.
inline constexpr std::string_view kWarnConfigDropWithoutMerge = "NH0008";

// ── Import (general) ──────────────────────────────────────────────────────────
inline constexpr std::string_view kFatalImportFileNotFound = "NH0100";
inline constexpr std::string_view kFatalImportFormatUnknown = "NH0101";
inline constexpr std::string_view kWarnImportUnknownShape = "NH0102";
inline constexpr std::string_view kWarnImportNoMaterial = "NH0103";
// Debug: what an importer produced, as counts. A library cannot print this to
// stderr — a Python session or an embedding application would get it whether it
// asked or not — so it goes down the same channel as everything else and the
// caller decides.
inline constexpr std::string_view kDebugImportStats = "NH0104";

// ── Scene operations ──────────────────────────────────────────────────────────
// Info: how much deduplication actually merged. Reported rather than discarded
// because "dedup ran" and "dedup did something" are different facts, and only
// the pass knows which happened.
inline constexpr std::string_view kInfoDedupMerged = "NH0200";

// ── TGeo importer ─────────────────────────────────────────────────────────────
inline constexpr std::string_view kFatalTgeoOpenFailed = "NH0300";
inline constexpr std::string_view kWarnTgeoUnknownShape = "NH0301";

// ── Selection engine ──────────────────────────────────────────────────────────
inline constexpr std::string_view kWarnSelectionOrphan = "NH0400";
// Fatal: the rules select nothing, so `prune` declines to act. The scene it
// would return is "the rules *not* applied", which is not what selection
// promised — see docs/error-model.md.
inline constexpr std::string_view kFatalSelectionRootDropped = "NH0401";

// ── Tessellation ──────────────────────────────────────────────────────────────
// Both errors here are partial results: the node named in the diagnostic ends
// up without a mesh binding and the rest of the scene is still a scene.
inline constexpr std::string_view kErrTessUnknownShape = "NH0500";
inline constexpr std::string_view kWarnTessBooleanSkipped = "NH0501";
inline constexpr std::string_view kWarnTessBooleanBbox = "NH0502";
inline constexpr std::string_view kErrTessBooleanFail = "NH0503";
inline constexpr std::string_view kWarnTessMergeEmpty = "NH0505";
inline constexpr std::string_view kWarnTessBooleanManifoldFail = "NH0506";
inline constexpr std::string_view kWarnTessDefaultMaterial = "NH0507";
// Debug: reports how many exact-coincident, opposite-wound triangle pairs the
// drop_coincident_faces pass stripped out of a merge_descendants group (interior
// faces between stacked opaque slabs — see tessellation_pass.cpp).
inline constexpr std::string_view kDebugTessCoincidentRemoved = "NH0508";
// Warning: a merge_descendants node has many coincident interior faces that
// drop_coincident_faces would remove, but the option is off — a discoverability hint
// that the optimisation exists and applies here (see tessellation_pass.cpp).
inline constexpr std::string_view kWarnTessCoincidentCandidate = "NH0509";
// Debug: what the pass produced, as counts. Same reasoning as kDebugImportStats
// — this used to be printed to stderr from inside the library, which every
// consumer paid for whether or not it wanted it.
inline constexpr std::string_view kDebugTessStats = "NH0510";

// ── Export ────────────────────────────────────────────────────────────────────
inline constexpr std::string_view kFatalExportWriteFailed = "NH0600";

// ── Compute worker (web) ────────────────────────────────────────────────────────
// Fatal, but delivered as a value rather than thrown: the failure arrives from
// another thread of control, where there is no call left to unwind. It rides in
// `SceneBuildResult::failure`, which is the fatal channel materialised for an
// asynchronous result — never in that result's diagnostics.
inline constexpr std::string_view kFatalComputeWorker = "NH0700";

// ── Public API boundary ───────────────────────────────────────────────────────
// Raised by src/api/ only, for the failure that exists because there is a
// boundary: a verb handed a handle that refers to nothing.
//
// NH0801 ("this build has no such backend") used to sit here too, raised by
// `Config::read` on a `.lua` path in a build without the interpreter. There is
// no such build any more, and no other site ever raised it: a format whose
// backend is absent is simply not in its registry, so the string-dispatched
// entry points already answer with their own unknown-format code (#41 §5). It
// is left unassigned rather than reused, since a code that meant something else
// once is worse than a gap.
inline constexpr std::string_view kFatalApiInvalidHandle = "NH0800";

// ── CLI usage ─────────────────────────────────────────────────────────────────
// Raised by src/cli/ only, for the failures that belong to the *invocation*
// rather than to the pipeline: a combination of options that cannot be honoured,
// or a path the user typed that is not there.
//
// They exist because the CLI stopped calling `std::exit`. Every one of these
// used to be a bare message on stderr followed by termination, which inside a
// library is the caller's process ending mid-call — so each became a thrown
// `Error`, and an `Error` needs a code. Deliberately three rather than one per
// site: what a caller could branch on is which *kind* of invocation failed, not
// which line noticed.
inline constexpr std::string_view kFatalCliUsage = "NH0900";
inline constexpr std::string_view kFatalCliPathNotFound = "NH0901";
inline constexpr std::string_view kFatalCliFileOpen = "NH0902";

// ── Web viewer runtime ────────────────────────────────────────────────────────
// The wasm runtime is built by a different toolchain than the library that
// serves it, so "where is it" and "is it the right one" are two failures, not
// one, and they have different remedies: find or install a runtime, versus
// match the two halves that disagree. A caller that wants to say `pip install
// nodehammer-web` on the first and `rebuild` on the second needs to tell them
// apart, which is the whole reason there are two codes here.
inline constexpr std::string_view kFatalWebRuntimeNotFound = "NH1000";
inline constexpr std::string_view kFatalWebRuntimeSchema = "NH1001";
inline constexpr std::string_view kFatalWebServeBind = "NH1002";
inline constexpr std::string_view kFatalWebStage = "NH1003";

} // namespace nodehammer::codes

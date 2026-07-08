#pragma once

#include <string_view>

// NH diagnostic code constants.
// Ranges:
//   NH0001–NH0099  config
//   NH0100–NH0199  import (general)
//   NH0300–NH0399  TGeo importer
//   NH0400–NH0499  selection engine
//   NH0500–NH0599  tessellation
//   NH0600–NH0699  export

namespace nodehammer::codes {

// ── Config ────────────────────────────────────────────────────────────────────
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
inline constexpr std::string_view kErrImportFileNotFound = "NH0100";
inline constexpr std::string_view kErrImportFormatUnknown = "NH0101";
inline constexpr std::string_view kWarnImportUnknownShape = "NH0102";
inline constexpr std::string_view kWarnImportNoMaterial = "NH0103";

// ── TGeo importer ─────────────────────────────────────────────────────────────
inline constexpr std::string_view kErrTgeoOpenFailed = "NH0300";
inline constexpr std::string_view kWarnTgeoUnknownShape = "NH0301";

// ── Selection engine ──────────────────────────────────────────────────────────
inline constexpr std::string_view kWarnSelectionOrphan = "NH0400";
inline constexpr std::string_view kErrSelectionRootDropped = "NH0401";

// ── Tessellation ──────────────────────────────────────────────────────────────
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

// ── Export ────────────────────────────────────────────────────────────────────
inline constexpr std::string_view kErrExportWriteFailed = "NH0600";

// ── Compute worker (web) ────────────────────────────────────────────────────────
inline constexpr std::string_view kErrComputeWorker = "NH0700";

} // namespace nodehammer::codes

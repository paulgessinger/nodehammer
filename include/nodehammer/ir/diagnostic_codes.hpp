#pragma once

#include <string_view>

// NH diagnostic code constants.
// Ranges:
//   NH0001–NH0099  config
//   NH0100–NH0199  import (general)
//   NH0200–NH0299  GDML importer
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

// ── Import (general) ──────────────────────────────────────────────────────────
inline constexpr std::string_view kErrImportFileNotFound = "NH0100";
inline constexpr std::string_view kErrImportFormatUnknown = "NH0101";
inline constexpr std::string_view kWarnImportUnknownShape = "NH0102";
inline constexpr std::string_view kWarnImportNoMaterial = "NH0103";

// ── GDML importer ─────────────────────────────────────────────────────────────
inline constexpr std::string_view kErrGdmlParseFailed = "NH0200";
inline constexpr std::string_view kWarnGdmlUnknownSolid = "NH0201";

// ── TGeo importer ─────────────────────────────────────────────────────────────
inline constexpr std::string_view kErrTgeoOpenFailed = "NH0300";
inline constexpr std::string_view kWarnTgeoUnknownShape = "NH0301";

// ── Selection engine ──────────────────────────────────────────────────────────
inline constexpr std::string_view kWarnSelectionOrphan = "NH0400";

// ── Tessellation ──────────────────────────────────────────────────────────────
inline constexpr std::string_view kErrTessUnknownShape = "NH0500";
inline constexpr std::string_view kWarnTessBooleanSkipped = "NH0501";
inline constexpr std::string_view kWarnTessBooleanBbox = "NH0502";
inline constexpr std::string_view kErrTessBooleanFail = "NH0503";

// ── Export ────────────────────────────────────────────────────────────────────
inline constexpr std::string_view kErrExportWriteFailed = "NH0600";

} // namespace nodehammer::codes

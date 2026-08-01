#pragma once

// The Semantic IR handle: the source-faithful scene graph, as imported.
//
// Tier A + Tier B. Together with diagnostics.hpp this is the entire connector
// surface (#41 §3), which is why the tessellation and export verbs are *not*
// members here: a `tessellate` member would drag SceneConfig and RenderScene
// into a header that must not know they exist. Cross-type verbs live in
// build.hpp instead (#41 §4).

#include <nodehammer/api.hpp>
#include <nodehammer/diagnostics.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// Declared unconditionally, defined only in a build with ROOT (#41 §5). The
/// forward declaration is at global scope because that is where ROOT puts the
/// class; the precedent is the internal TGeo importer, which has done exactly
/// this since long before this API existed.
class TGeoManager;

namespace nodehammer {

struct SemanticResult;

/// An imported geometry, before any selection, deduplication or tessellation.
///
/// Read-only and cheap to copy: the handle is a reference to an immutable
/// scene, so passing one around costs a pointer, and the verbs in build.hpp
/// return new handles rather than mutating this one.
///
/// **`diags` is what says whether to trust a result; `valid()` only says whether
/// there is one to look at.** The two are independent on purpose. An importer
/// may hand back a partly-imported geometry alongside errors, and this API does
/// not decide on your behalf that such a scene is worth nothing — it is often
/// exactly what you want to inspect. Always check `diags.hasErrors()`.
class SemanticScene {
  public:
    /// Options for the read verbs. Nested because they appear in no signature
    /// without the enclosing type (#41 §4).
    struct ReadOptions {
        /// Explicit input format — one of the names `formats()` reports, e.g.
        /// "synthetic", "json", "flatbuffer", or "tgeo"/"dd4hep" where the
        /// backend is present. Empty means "infer from the path extension". A
        /// name this build does not have is reported as a diagnostic, not a link
        /// error: the format is a value, so nothing earlier could have known.
        std::string format;
    };

    struct WriteOptions {
        /// Explicit output format: "json" or "nhb". Empty means "infer from the
        /// path extension".
        ///
        /// The write side spells the FlatBuffer format "nhb" where the read side
        /// spells it "flatbuffer" — the two registries were named independently
        /// and `formats()` reports both rather than picking a winner.
        std::string format;
    };

    /// Read a geometry file. The format comes from `options.format` when set,
    /// otherwise from the extension.
    [[nodiscard]] NH_API static SemanticResult read(const std::filesystem::path &path,
                                                    const ReadOptions &options = {});

    /// Read uncompressed `.nhb` bytes that never touched a filesystem — the
    /// wire form used by the connector and by the browser build.
    [[nodiscard]] NH_API static SemanticResult read(std::span<const std::byte> nhb);

    /// Traverse a caller-owned `TGeoManager`. Never touches `gGeoManager`.
    ///
    /// Declared in every build; defined only where ROOT is present, so calling
    /// it against a build without TGeo fails at your final link rather than at
    /// run time — the earliest point detectable without a macro in this header
    /// (#41 §5).
    ///
    /// Takes no `ReadOptions`, unlike its siblings: the format is settled by
    /// the argument's type, and the struct's only field selects a format. A
    /// knob with one legal value is not a knob.
    [[nodiscard]] NH_API static SemanticResult read(TGeoManager &manager);

    /// Format names this build can read or write, in registration order and
    /// without duplicates. The runtime capability query for the optional
    /// backends: `"tgeo"` appears here only in a build that has ROOT.
    [[nodiscard]] NH_API static std::vector<std::string> formats();

    /// Write the scene. Format from `options.format` when set, otherwise from
    /// the extension. A `.zst` suffix compresses.
    [[nodiscard]] NH_API DiagnosticList write(const std::filesystem::path &path,
                                              const WriteOptions &options = {}) const;

    /// The scene as `.nhb` bytes. Empty when the handle is invalid.
    [[nodiscard]] NH_API std::vector<std::byte> toNhb() const;

    /// True when this handle refers to a scene at all. False only for a
    /// default-constructed or moved-from handle, and for the few failures that
    /// produce no scene whatsoever (an unresolvable format, say).
    ///
    /// Not a success check — a read that reported errors can still return a
    /// scene, and does. Use `diags.hasErrors()` for that.
    [[nodiscard]] NH_API bool valid() const noexcept;

    [[nodiscard]] NH_API std::size_t nodeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t logVolCount() const noexcept;
    [[nodiscard]] NH_API std::size_t shapeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t materialCount() const noexcept;

    /// Opaque state. `shared_ptr<const>` inside, value outside (#41 principle
    /// 4); public because `Impl` being undefined here is what makes the handle
    /// opaque, so privacy would add a friend declaration and protect nothing.
    /// See `DiagnosticList::impl` for the full argument.
    struct Impl;
    std::shared_ptr<const Impl> impl;
};

/// Named after its type rather than a generic `scene`, so a structured binding
/// reads correctly at the call site (#41 §9).
struct SemanticResult {
    SemanticScene scene;
    DiagnosticList diags;
};

} // namespace nodehammer

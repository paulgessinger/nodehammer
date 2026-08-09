#pragma once

// The Semantic IR handle: the source-faithful scene graph, as imported.
//
// Tier A + Tier B. Together with diagnostics.hpp this is the entire connector
// surface (#41 §3), which is why the tessellation and export verbs are *not*
// members here: a `tessellate` member would drag SceneConfig and RenderScene
// into a header that must not know they exist. Cross-type verbs live in
// build.hpp instead (#41 §4).

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/visibility.hpp>

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
/// A call that could not read its input throws `Error`; there is no "invalid
/// result" to inspect, so `valid()` is false only for a default-constructed or
/// moved-from handle. `diags` carries what the import *observed* — an unknown
/// shape, a missing material — alongside the scene it did produce.
class SemanticScene {
  public:
    /// Options for the read verbs. Nested because they appear in no signature
    /// without the enclosing type (#41 §4).
    struct ReadOptions {
        /// Explicit input format — one of the names `formats()` reports, e.g.
        /// "synthetic", "json", "flatbuffer", or "tgeo"/"dd4hep" where the
        /// backend is present. Empty means "infer from the path extension". A
        /// name this build does not have throws rather than failing to link: the
        /// format is a value, so nothing earlier could have known.
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
    ///
    /// A view over storage that lives as long as the library, not a container
    /// handed across the boundary — the set is fixed once the process starts, so
    /// there is nothing to own and nothing to free.
    [[nodiscard]] NH_API static std::span<const std::string_view> formats();

    /// Write the scene. Format from `options.format` when set, otherwise from
    /// the extension. A `.zst` suffix compresses.
    ///
    /// Returns nothing, and that is the whole contract: it either wrote the file
    /// or threw saying why. No exporter in the tree has a non-fatal observation
    /// to make about a file it wrote successfully, so a `DiagnosticList` here
    /// could only ever have come back empty (docs/error-model.md).
    NH_API void write(const std::filesystem::path &path, const WriteOptions &options = {}) const;

    /// The scene as `.nhb` bytes. Throws on an empty handle.
    [[nodiscard]] NH_API std::vector<std::byte> toNhb() const;

    /// True when this handle refers to a scene at all — false only for a
    /// default-constructed or moved-from one, since anything that could not
    /// produce a scene threw instead of returning an empty handle.
    ///
    /// The observers below answer for an empty handle; everything that would
    /// have to dereference one (`write`, `toNhb`, the verbs) throws.
    [[nodiscard]] NH_API bool valid() const noexcept;

    [[nodiscard]] NH_API std::size_t nodeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t logVolCount() const noexcept;
    [[nodiscard]] NH_API std::size_t shapeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t materialCount() const noexcept;

    /// Opaque state. `shared_ptr<const>` inside, value outside (#41 principle
    /// 4), private behind a constructor and a getter — see `DiagnosticList` for
    /// the full argument.
    struct Impl;

    SemanticScene() noexcept = default;

    /// Adopt state the library built.
    explicit SemanticScene(std::shared_ptr<const Impl> impl) noexcept;

    /// The state behind a live handle. Throws `Error` when `valid()` is false,
    /// since there is nothing to return a reference to.
    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
};

/// Named after its type rather than a generic `scene`, so a structured binding
/// reads correctly at the call site (#41 §9).
struct SemanticResult {
    SemanticScene scene;
    DiagnosticList diags;
};

} // namespace nodehammer

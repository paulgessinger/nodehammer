#pragma once

// The Render IR handle: GPU-ready triangle geometry, materials, and the node
// tree that instances them. What tessellation produces and what an exporter
// consumes.
//
// Tier A only — it needs OutputConfig, and through the exporters it reaches
// tinygltf, neither of which belongs in the connector amalgamation.

#include <nodehammer/api.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nodehammer {

struct RenderResult;

/// A tessellated scene. Same handle contract as `SemanticScene`: read-only,
/// cheap to copy, and carrying whatever the producing call made — check the
/// accompanying diagnostics rather than `valid()` to learn whether to trust it.
class RenderScene {
  public:
    struct WriteOptions {
        /// Explicit output format — one of the names `formats()` reports, which
        /// today are "nhr", "gltf" and "obj". Empty means "infer from the path
        /// extension". Dispatch only: everything that *tunes* a write lives in
        /// `OutputConfig`.
        ///
        /// Note that GLB is the `.glb` flavour of the "gltf" writer rather than
        /// a format name of its own, so it is selected by the extension — the
        /// same thing `nodehammer convert --output-format` accepts, and the same
        /// place its `glb` gap lives.
        std::string format;
    };

    /// Read `.nhr` (optionally `.nhr.zst`).
    [[nodiscard]] NH_API static RenderResult read(const std::filesystem::path &path);

    /// Read `.nhr` bytes directly — the form that crosses the browser's worker
    /// boundary.
    [[nodiscard]] NH_API static RenderResult read(std::span<const std::byte> nhr);

    /// Format names this build can read or write, in registration order and
    /// without duplicates.
    [[nodiscard]] NH_API static std::vector<std::string> formats();

    /// Write the scene.
    ///
    /// `output` supplies the `[export.*]` tuning; omitting it uses each
    /// format's built-in defaults, which is exactly what a default-constructed
    /// document resolves to. The precedence rules — format defaults, then the
    /// matching `[export.<fmt>]` table field by field, then GLB's fallback to
    /// `[export.gltf]` — are the same single implementation the CLI uses, so a
    /// config that sets `unit_scale` cannot be honoured by `build` and ignored
    /// here (#41 §3).
    [[nodiscard]] NH_API DiagnosticList write(const std::filesystem::path &path,
                                              const OutputConfig &output = {},
                                              const WriteOptions &options = {}) const;

    /// The scene as `.nhr` bytes. Empty when the handle is invalid.
    [[nodiscard]] NH_API std::vector<std::byte> toNhr() const;

    /// True when this handle refers to a scene at all — not a success check.
    /// See `SemanticScene::valid`.
    [[nodiscard]] NH_API bool valid() const noexcept;

    [[nodiscard]] NH_API std::size_t nodeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t meshCount() const noexcept;
    [[nodiscard]] NH_API std::size_t materialCount() const noexcept;
    [[nodiscard]] NH_API std::size_t triangleCount() const noexcept;

    /// Opaque state — see `DiagnosticList::impl`.
    struct Impl;
    std::shared_ptr<const Impl> impl;
};

struct RenderResult {
    RenderScene scene;
    DiagnosticList diags;
};

} // namespace nodehammer

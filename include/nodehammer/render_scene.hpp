#pragma once

// The Render IR handle: GPU-ready triangle geometry, materials, and the node
// tree that instances them. What tessellation produces and what an exporter
// consumes.
//
// Tier A only — it needs OutputConfig, and through the exporters it reaches
// tinygltf, neither of which belongs in the connector amalgamation.

#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/visibility.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nodehammer {

struct RenderResult;

/// A tessellated scene. Same handle contract as `SemanticScene`: read-only,
/// cheap to copy, produced by a call that either succeeds or throws.
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
    ///
    /// Returns the scene rather than a `RenderResult`: reading `.nhr` either
    /// produces the scene or throws, and there is nothing non-fatal to observe
    /// on the way. A result type here could only ever have carried an empty
    /// list (docs/error-model.md).
    [[nodiscard]] NH_API static RenderScene read(const std::filesystem::path &path);

    /// Read `.nhr` bytes directly — the form that crosses the browser's worker
    /// boundary.
    [[nodiscard]] NH_API static RenderScene read(std::span<const std::byte> nhr);

    /// Format names this build can read or write, in registration order and
    /// without duplicates. A view over library-lifetime storage — see
    /// `SemanticScene::formats`.
    [[nodiscard]] NH_API static std::span<const std::string_view> formats();

    /// Write the scene.
    ///
    /// `output` supplies the `[export.*]` tuning; omitting it uses each
    /// format's built-in defaults, which is exactly what a default-constructed
    /// document resolves to. The precedence rules — format defaults, then the
    /// matching `[export.<fmt>]` table field by field, then GLB's fallback to
    /// `[export.gltf]` — are the same single implementation the CLI uses, so a
    /// config that sets `unit_scale` cannot be honoured by `build` and ignored
    /// here (#41 §3).
    ///
    /// Returns nothing — see `SemanticScene::write`.
    NH_API void write(const std::filesystem::path &path, const OutputConfig &output = {},
                      const WriteOptions &options = {}) const;

    /// The scene as `.nhr` bytes. Throws on an empty handle.
    [[nodiscard]] NH_API std::vector<std::byte> toNhr() const;

    /// True when this handle refers to a scene at all. See `SemanticScene::valid`.
    [[nodiscard]] NH_API bool valid() const noexcept;

    [[nodiscard]] NH_API std::size_t nodeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t meshCount() const noexcept;
    [[nodiscard]] NH_API std::size_t materialCount() const noexcept;
    [[nodiscard]] NH_API std::size_t triangleCount() const noexcept;

    /// Opaque state — see `DiagnosticList::Impl`.
    struct Impl;

    RenderScene() noexcept = default;

    /// Adopt state the library built.
    explicit RenderScene(std::shared_ptr<const Impl> impl) noexcept;

    /// The state behind a live handle. Throws `Error` when `valid()` is false.
    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
};

struct RenderResult {
    RenderScene scene;
    DiagnosticList diags;
};

} // namespace nodehammer

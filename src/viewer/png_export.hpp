#pragma once

#include <cstdint>
#include <vector>

namespace nodehammer::viewer {

/// User-facing configuration for a high-resolution PNG export.
///
/// The frame is rendered at `(out_width * supersample) × (out_height * supersample)`
/// with every quality knob dialled to its maximum, then box-downscaled to
/// `out_width × out_height` and encoded as a PNG. The supersample factor is the
/// antialiasing lever (pure SSAA); the output dimensions set the file size and
/// drive the camera aspect ratio for the exported frame (independent of the
/// live window's aspect).
struct PngExportSettings {
    std::uint32_t out_width{1920};
    std::uint32_t out_height{1080};
    /// Internal render = output × this. 1 = no supersampling; clamped to
    /// [1, kMaxSupersample] and further reduced at request time if the internal
    /// resolution would exceed the backend's maximum texture size.
    std::uint32_t supersample{2};

    static constexpr std::uint32_t kMaxSupersample = 4;
};

/// Box-downscale a tightly-packed RGBA8 image by an integer factor in each
/// axis. `src` holds `src_w * src_h * 4` bytes (top-left origin); the result
/// holds `(src_w / factor) * (src_h / factor) * 4` bytes. `factor` must be
/// >= 1 and divide both dimensions (the exporter guarantees this by deriving
/// the source size as output × factor). A factor of 1 is a straight copy.
///
/// Each destination pixel is the unweighted average of its `factor × factor`
/// source block — the exact resolve for integer-ratio supersampling.
std::vector<std::uint8_t> downscaleBoxRgba8(const std::vector<std::uint8_t> &src,
                                            std::uint32_t src_w, std::uint32_t src_h,
                                            std::uint32_t factor);

/// Encode a tightly-packed RGBA8 image (top-left origin) as a PNG into a byte
/// buffer. Returns an empty vector on failure.
std::vector<std::uint8_t> encodePngRgba8(const std::vector<std::uint8_t> &rgba, std::uint32_t width,
                                         std::uint32_t height);

} // namespace nodehammer::viewer

#pragma once

#include <sokol_gfx.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace nodehammer::viewer {

enum class ReadbackStatus { Idle, Pending, Ready, Failed };

/// Backend-specific GPU→CPU readback of a 2D color image, used by the PNG
/// export path. The public interface is uniform; the implementation is selected
/// at build time (one TU per graphics backend — Metal / WebGPU / WebGL2). The
/// remaining native backends (D3D11 / Vulkan / GLCORE) are intentionally a
/// build-time #error until someone can implement and test them.
///
/// Usage, driven across frames by the exporter:
///   begin(img, w, h, fmt);             // issue the copy/blit/map
///   while (poll(out) == Pending) {}    // advance until done; `out` filled on Ready
///
/// The image's GPU writes MUST already be complete when begin() is called — the
/// exporter waits a few frames after the capture pass so sokol's command buffer
/// for that frame has drained.
///
/// On Ready, `out` holds tightly-packed RGBA8 with a top-left origin: each
/// backend swizzles BGRA→RGBA and flips bottom-left framebuffers as needed, so
/// callers get a consistent layout regardless of backend. Size is w*h*4.
class ImageReadback {
  public:
    ImageReadback();
    ~ImageReadback();
    ImageReadback(const ImageReadback &) = delete;
    ImageReadback &operator=(const ImageReadback &) = delete;

    /// Kick off the readback of `image` (w×h, pixel format `format`). Returns
    /// false if the backend can't service the request (treated as failure).
    [[nodiscard]] bool begin(sg_image image, std::uint32_t width, std::uint32_t height,
                             sg_pixel_format format);

    /// Advance any async work and report status. On Ready, moves the decoded
    /// RGBA8 pixels into `out`.
    ReadbackStatus poll(std::vector<std::uint8_t> &out);

    /// Drop any in-flight state / buffers and return to Idle.
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer

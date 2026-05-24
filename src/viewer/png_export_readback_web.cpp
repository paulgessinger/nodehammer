#include "png_export_readback.hpp"

// Web (Emscripten) GPU→CPU readback for the PNG export. This single TU is
// compiled into BOTH web viewer libs (WebGPU and WebGL2), so it branches on the
// sokol backend define.
//
// TODO(png-export, phase 2): implement the actual readback.
//   * SOKOL_WGPU: wgpuCommandEncoderCopyTextureToBuffer into a MAP_READ buffer
//     (sokol exposes sg_wgpu_device/_queue and the WGPUTexture via
//     sg_wgpu_query_image_info), then wgpuBufferMapAsync + poll the future.
//     Mind the 256-byte bytesPerRow alignment and BGRA→RGBA swizzle.
//   * SOKOL_GLES3: bind the texture to a framebuffer and glReadPixels; the GL
//     framebuffer is bottom-left origin, so flip rows for the top-left contract.
//
// Until then begin() reports failure so the exporter surfaces a clear "not
// supported on this build" message instead of producing a blank/garbage PNG.

namespace nodehammer::viewer {

struct ImageReadback::Impl {};

ImageReadback::ImageReadback() : impl_(std::make_unique<Impl>()) {}
ImageReadback::~ImageReadback() = default;

bool ImageReadback::begin(sg_image /*image*/, std::uint32_t /*width*/, std::uint32_t /*height*/,
                          sg_pixel_format /*format*/) {
    return false;
}

ReadbackStatus ImageReadback::poll(std::vector<std::uint8_t> & /*out*/) {
    return ReadbackStatus::Failed;
}

void ImageReadback::reset() {}

} // namespace nodehammer::viewer

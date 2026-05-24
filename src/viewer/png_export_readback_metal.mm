#include "png_export_readback.hpp"

#import <Metal/Metal.h>

#include <cstdint>
#include <utility>
#include <vector>

// Metal GPU→CPU readback for the PNG export. sokol exposes the native MTLDevice
// (sg_mtl_device) and the MTLTexture behind an sg_image (sg_mtl_query_image_info),
// which is all we need: blit the render-target texture into a shared MTLBuffer on
// our own command queue and wait for it. The exporter only calls begin() a few
// frames after the capture pass, by which point sokol's command buffer for that
// frame has completed (Metal keeps SG_NUM_INFLIGHT_FRAMES in flight and a later
// frame's sg_begin_pass has blocked on the capture frame's semaphore), so the
// source texture holds stable, finished pixels and a synchronous blit is race-free.
//
// The viewer's Objective-C++ is compiled under manual reference counting (see
// platform_macos.mm), so owned objects (the command queue) are released explicitly.

namespace nodehammer::viewer {

namespace {
std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1u) / a * a; }
} // namespace

struct ImageReadback::Impl {
    id<MTLCommandQueue> queue{nil};
    ReadbackStatus status{ReadbackStatus::Idle};
    std::vector<std::uint8_t> pixels;

    ~Impl() {
        if (queue != nil) {
            [queue release];
            queue = nil;
        }
    }
};

ImageReadback::ImageReadback() : impl_(std::make_unique<Impl>()) {}
ImageReadback::~ImageReadback() = default;

bool ImageReadback::begin(sg_image image, std::uint32_t width, std::uint32_t height,
                          sg_pixel_format format) {
    impl_->status = ReadbackStatus::Failed;
    impl_->pixels.clear();
    if (width == 0 || height == 0) {
        return false;
    }

    auto dev = (__bridge id<MTLDevice>)sg_mtl_device();
    if (dev == nil) {
        return false;
    }
    sg_mtl_image_info info = sg_mtl_query_image_info(image);
    const int slot = info.active_slot;
    const void *tex_ptr = (slot >= 0 && slot < SG_NUM_INFLIGHT_FRAMES) ? info.tex[slot] : nullptr;
    if (tex_ptr == nullptr) {
        tex_ptr = info.tex[0];
    }
    auto tex = (__bridge id<MTLTexture>)tex_ptr;
    if (tex == nil) {
        return false;
    }

    if (impl_->queue == nil) {
        impl_->queue = [dev newCommandQueue];
    }
    if (impl_->queue == nil) {
        return false;
    }

    // Metal's copyFromTexture:toBuffer: wants the destination row pitch to be a
    // multiple of the texture's pixel size; round up to 256 to satisfy every GPU.
    const std::uint32_t row_bytes = alignUp(width * 4u, 256u);
    const std::size_t buf_len = static_cast<std::size_t>(row_bytes) * height;
    id<MTLBuffer> buf = [dev newBufferWithLength:buf_len options:MTLResourceStorageModeShared];
    if (buf == nil) {
        return false;
    }

    id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:row_bytes
        destinationBytesPerImage:buf_len];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];

    const bool bgra = (format == SG_PIXELFORMAT_BGRA8);
    const auto *src = static_cast<const std::uint8_t *>([buf contents]);
    impl_->pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *srow = src + static_cast<std::size_t>(y) * row_bytes;
        std::uint8_t *drow = impl_->pixels.data() + static_cast<std::size_t>(y) * width * 4u;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t *s = srow + static_cast<std::size_t>(x) * 4u;
            std::uint8_t *d = drow + static_cast<std::size_t>(x) * 4u;
            if (bgra) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
            } else {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
            }
        }
    }
    [buf release];

    impl_->status = ReadbackStatus::Ready;
    return true;
}

ReadbackStatus ImageReadback::poll(std::vector<std::uint8_t> &out) {
    if (impl_->status == ReadbackStatus::Ready) {
        out = std::move(impl_->pixels);
        impl_->pixels.clear();
        impl_->status = ReadbackStatus::Idle;
        return ReadbackStatus::Ready;
    }
    return impl_->status;
}

void ImageReadback::reset() {
    impl_->status = ReadbackStatus::Idle;
    impl_->pixels.clear();
}

} // namespace nodehammer::viewer

#include "png_export_readback.hpp"

#include <webgpu/webgpu.h>

#include <cstdint>
#include <utility>
#include <vector>

// WebGPU GPU→CPU readback for the PNG export. sokol exposes the device, queue
// and the WGPUTexture behind an sg_image (sg_wgpu_device / sg_wgpu_queue /
// sg_wgpu_query_image_info). WebGPU can't map a texture directly, so we:
//   1. copy the texture into a MapRead|CopyDst buffer (CopyTextureToBuffer),
//   2. submit, then mapAsync the buffer,
//   3. poll across frames until the map callback fires (AllowSpontaneous — the
//      browser/Dawn event loop resolves it between our render frames),
//   4. de-pad the 256-byte-aligned rows and swizzle BGRA→RGBA on copy-out.
//
// The catch: WebGPU textures need an explicit CopySrc usage that sokol never
// sets on its render targets. makeReadbackColorImage() below works around that
// by injecting a self-created WGPUTexture (with CopySrc) for the export target;
// the rest of the pipeline renders into it normally.

namespace nodehammer::viewer {

namespace {
std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1u) / a * a; }

// Map the subset of sokol pixel formats the export target can use to WebGPU
// texture formats. The export target uses the swapchain color format, which on
// a WebGPU canvas is almost always bgra8unorm (occasionally rgba8unorm).
WGPUTextureFormat wgpuColorFormat(sg_pixel_format fmt) {
    switch (fmt) {
    case SG_PIXELFORMAT_RGBA8:
        return WGPUTextureFormat_RGBA8Unorm;
    case SG_PIXELFORMAT_BGRA8:
        return WGPUTextureFormat_BGRA8Unorm;
    default:
        return WGPUTextureFormat_Undefined;
    }
}
} // namespace

struct ImageReadback::Impl {
    WGPUBuffer buffer{nullptr};
    std::size_t buf_len{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t row_bytes{0};
    bool bgra{false};
    ReadbackStatus status{ReadbackStatus::Idle};
    bool map_done{false};
    WGPUMapAsyncStatus map_status{WGPUMapAsyncStatus_Error};
    std::vector<std::uint8_t> pixels;

    void releaseBuffer() {
        if (buffer != nullptr) {
            wgpuBufferRelease(buffer);
            buffer = nullptr;
        }
    }

    ~Impl() { releaseBuffer(); }

    // WGPUBufferMapCallback. userdata1 is the owning Impl, which the exporter
    // keeps alive for the whole async readback.
    static void onMapped(WGPUMapAsyncStatus status, WGPUStringView /*message*/, void *ud1,
                         void * /*ud2*/) {
        auto *self = static_cast<Impl *>(ud1);
        if (self == nullptr) {
            return;
        }
        self->map_status = status;
        self->map_done = true;
    }
};

ImageReadback::ImageReadback() : impl_(std::make_unique<Impl>()) {}
ImageReadback::~ImageReadback() = default;

bool ImageReadback::begin(sg_image image, std::uint32_t width, std::uint32_t height,
                          sg_pixel_format format) {
    impl_->releaseBuffer();
    impl_->pixels.clear();
    impl_->map_done = false;
    impl_->map_status = WGPUMapAsyncStatus_Error;
    impl_->status = ReadbackStatus::Failed;
    if (width == 0 || height == 0) {
        return false;
    }

    auto dev = static_cast<WGPUDevice>(const_cast<void *>(sg_wgpu_device()));
    auto queue = static_cast<WGPUQueue>(const_cast<void *>(sg_wgpu_queue()));
    if (dev == nullptr || queue == nullptr) {
        return false;
    }
    sg_wgpu_image_info info = sg_wgpu_query_image_info(image);
    auto tex = static_cast<WGPUTexture>(const_cast<void *>(info.tex));
    if (tex == nullptr) {
        return false;
    }

    // WebGPU requires the buffer copy row pitch to be a multiple of 256.
    const std::uint32_t row_bytes = alignUp(width * 4u, 256u);
    const std::uint64_t buf_len = static_cast<std::uint64_t>(row_bytes) * height;

    WGPUBufferDescriptor bdesc = {};
    bdesc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    bdesc.size = buf_len;
    bdesc.mappedAtCreation = false;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(dev, &bdesc);
    if (buf == nullptr) {
        return false;
    }

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev, nullptr);

    WGPUTexelCopyTextureInfo src = {};
    src.texture = tex;
    src.mipLevel = 0;
    src.origin = {0, 0, 0};
    src.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyBufferInfo dst = {};
    dst.layout.offset = 0;
    dst.layout.bytesPerRow = row_bytes;
    dst.layout.rowsPerImage = height;
    dst.buffer = buf;

    WGPUExtent3D ext = {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    impl_->buffer = buf;
    impl_->buf_len = static_cast<std::size_t>(buf_len);
    impl_->width = width;
    impl_->height = height;
    impl_->row_bytes = row_bytes;
    impl_->bgra = (format == SG_PIXELFORMAT_BGRA8);
    impl_->status = ReadbackStatus::Pending;

    WGPUBufferMapCallbackInfo cbinfo = {};
    cbinfo.mode = WGPUCallbackMode_AllowSpontaneous;
    cbinfo.callback = &Impl::onMapped;
    cbinfo.userdata1 = impl_.get();
    cbinfo.userdata2 = nullptr;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, impl_->buf_len, cbinfo);
    return true;
}

ReadbackStatus ImageReadback::poll(std::vector<std::uint8_t> &out) {
    if (impl_->status == ReadbackStatus::Pending) {
        if (!impl_->map_done) {
            return ReadbackStatus::Pending;
        }
        // Map resolved — read the buffer, de-pad rows, swizzle, then release.
        const std::uint8_t *src = nullptr;
        if (impl_->map_status == WGPUMapAsyncStatus_Success && impl_->buffer != nullptr) {
            src = static_cast<const std::uint8_t *>(
                wgpuBufferGetConstMappedRange(impl_->buffer, 0, impl_->buf_len));
        }
        if (src != nullptr) {
            const std::uint32_t w = impl_->width;
            const std::uint32_t h = impl_->height;
            impl_->pixels.resize(static_cast<std::size_t>(w) * h * 4u);
            for (std::uint32_t y = 0; y < h; ++y) {
                const std::uint8_t *srow = src + static_cast<std::size_t>(y) * impl_->row_bytes;
                std::uint8_t *drow = impl_->pixels.data() + static_cast<std::size_t>(y) * w * 4u;
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint8_t *s = srow + static_cast<std::size_t>(x) * 4u;
                    std::uint8_t *d = drow + static_cast<std::size_t>(x) * 4u;
                    if (impl_->bgra) {
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
            impl_->status = ReadbackStatus::Ready;
        } else {
            impl_->status = ReadbackStatus::Failed;
        }
        if (impl_->buffer != nullptr) {
            wgpuBufferUnmap(impl_->buffer);
        }
        impl_->releaseBuffer();
    }

    if (impl_->status == ReadbackStatus::Ready) {
        out = std::move(impl_->pixels);
        impl_->pixels.clear();
        impl_->status = ReadbackStatus::Idle;
        return ReadbackStatus::Ready;
    }
    return impl_->status;
}

void ImageReadback::reset() {
    impl_->releaseBuffer();
    impl_->pixels.clear();
    impl_->map_done = false;
    impl_->status = ReadbackStatus::Idle;
}

sg_image makeReadbackColorImage(std::uint32_t width, std::uint32_t height, sg_pixel_format format) {
    auto dev = static_cast<WGPUDevice>(const_cast<void *>(sg_wgpu_device()));
    const WGPUTextureFormat wgpu_fmt = wgpuColorFormat(format);
    if (dev == nullptr || width == 0 || height == 0 || wgpu_fmt == WGPUTextureFormat_Undefined) {
        return sg_image{SG_INVALID_ID};
    }

    // Same usage sokol gives a render-target texture, plus CopySrc so the
    // readback's CopyTextureToBuffer is allowed against it.
    WGPUTextureDescriptor tdesc = {};
    tdesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                  WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    tdesc.dimension = WGPUTextureDimension_2D;
    tdesc.size = {width, height, 1};
    tdesc.format = wgpu_fmt;
    tdesc.mipLevelCount = 1;
    tdesc.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(dev, &tdesc);
    if (tex == nullptr) {
        return sg_image{SG_INVALID_ID};
    }

    sg_image_desc cdesc{};
    cdesc.usage.color_attachment = true;
    cdesc.width = static_cast<int>(width);
    cdesc.height = static_cast<int>(height);
    cdesc.pixel_format = format;
    cdesc.sample_count = 1;
    cdesc.label = "scene_color_export";
    cdesc.wgpu_texture = tex;
    sg_image img = sg_make_image(&cdesc);

    // sokol takes its own ref on the injected texture; drop ours so the texture
    // is freed when the sg_image is destroyed.
    wgpuTextureRelease(tex);
    return img;
}

} // namespace nodehammer::viewer

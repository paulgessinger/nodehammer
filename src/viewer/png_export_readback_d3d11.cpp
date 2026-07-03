#include "png_export_readback.hpp"

// D3D11 GPU→CPU readback for the PNG export.
//
// sokol exposes the device/context (sg_d3d11_device / sg_d3d11_device_context)
// and the source texture behind an sg_image (sg_d3d11_query_image_info). A
// render target can't be Map()'d directly, so we create a STAGING copy
// (D3D11_USAGE_STAGING + CPU_ACCESS_READ, no bind flags), CopyResource the
// render-target texture into it, Map it, and copy the rows out — de-padding by
// the mapped RowPitch. The copy + map are synchronous (Map blocks until the
// GPU work feeding the staging texture has completed), so begin() does all the
// work and poll() just hands back the result, mirroring the Metal path. The
// exporter only calls begin() a few frames after the capture pass, so the
// source texture already holds finished pixels.
//
// Two D3D11 specifics handled here:
//   * D3D11 textures are top-left origin (like Metal), so — unlike GL — there
//     is no row flip.
//   * The default D3D11 swapchain/offscreen color format is BGRA8, so the raw
//     texels come back BGRA and are swizzled to the RGBA8 top-left contract
//     when the source format is SG_PIXELFORMAT_BGRA8.

#include <d3d11.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

struct ImageReadback::Impl {
    ReadbackStatus status{ReadbackStatus::Idle};
    std::vector<std::uint8_t> pixels;
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

    auto *dev = static_cast<ID3D11Device *>(const_cast<void *>(sg_d3d11_device()));
    auto *ctx = static_cast<ID3D11DeviceContext *>(const_cast<void *>(sg_d3d11_device_context()));
    if (dev == nullptr || ctx == nullptr) {
        return false;
    }

    sg_d3d11_image_info info = sg_d3d11_query_image_info(image);
    auto *src_tex = static_cast<ID3D11Texture2D *>(const_cast<void *>(info.tex2d));
    auto *src_res = static_cast<ID3D11Resource *>(const_cast<void *>(info.res));
    if (src_tex == nullptr || src_res == nullptr) {
        return false;
    }

    // Match the source's format/dimensions exactly (CopyResource requires it),
    // but make the copy CPU-readable and unbound from the pipeline.
    D3D11_TEXTURE2D_DESC desc{};
    src_tex->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.MipLevels = 1;
    sdesc.ArraySize = 1;
    sdesc.SampleDesc.Count = 1;
    sdesc.SampleDesc.Quality = 0;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sdesc.MiscFlags = 0;

    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sdesc, nullptr, &staging)) || staging == nullptr) {
        return false;
    }

    ctx->CopyResource(staging, src_res);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return false;
    }

    const bool bgra = (format == SG_PIXELFORMAT_BGRA8);
    const auto *base = static_cast<const std::uint8_t *>(mapped.pData);
    impl_->pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *srow = base + static_cast<std::size_t>(y) * mapped.RowPitch;
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

    ctx->Unmap(staging, 0);
    staging->Release();

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

sg_image makeReadbackColorImage(std::uint32_t, std::uint32_t, sg_pixel_format) {
    // A normal color attachment can be CopyResource'd into a staging texture
    // as-is, so no special image is needed — let the caller allocate the
    // standard one.
    return sg_image{SG_INVALID_ID};
}

} // namespace nodehammer::viewer

#include <viewer/png_export.hpp>

#include <stb_image_write.h>

namespace nodehammer::viewer {

std::vector<std::uint8_t> downscaleBoxRgba8(const std::vector<std::uint8_t> &src,
                                            std::uint32_t src_w, std::uint32_t src_h,
                                            std::uint32_t factor) {
    if (factor <= 1) {
        return src; // identity — nothing to average
    }
    const std::uint32_t dst_w = src_w / factor;
    const std::uint32_t dst_h = src_h / factor;
    if (dst_w == 0 || dst_h == 0) {
        return src;
    }
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(dst_w) * dst_h * 4u);
    const std::uint32_t block = factor * factor;
    for (std::uint32_t dy = 0; dy < dst_h; ++dy) {
        for (std::uint32_t dx = 0; dx < dst_w; ++dx) {
            std::uint32_t acc[4] = {0, 0, 0, 0};
            const std::uint32_t sy0 = dy * factor;
            const std::uint32_t sx0 = dx * factor;
            for (std::uint32_t by = 0; by < factor; ++by) {
                const std::size_t row = (static_cast<std::size_t>(sy0 + by) * src_w + sx0) * 4u;
                for (std::uint32_t bx = 0; bx < factor; ++bx) {
                    const std::size_t p = row + static_cast<std::size_t>(bx) * 4u;
                    acc[0] += src[p + 0];
                    acc[1] += src[p + 1];
                    acc[2] += src[p + 2];
                    acc[3] += src[p + 3];
                }
            }
            const std::size_t o = (static_cast<std::size_t>(dy) * dst_w + dx) * 4u;
            // +block/2 rounds to nearest rather than truncating.
            dst[o + 0] = static_cast<std::uint8_t>((acc[0] + block / 2) / block);
            dst[o + 1] = static_cast<std::uint8_t>((acc[1] + block / 2) / block);
            dst[o + 2] = static_cast<std::uint8_t>((acc[2] + block / 2) / block);
            dst[o + 3] = static_cast<std::uint8_t>((acc[3] + block / 2) / block);
        }
    }
    return dst;
}

std::vector<std::uint8_t> encodePngRgba8(const std::vector<std::uint8_t> &rgba, std::uint32_t width,
                                         std::uint32_t height) {
    std::vector<std::uint8_t> out;
    if (width == 0 || height == 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4u) {
        return out;
    }
    const auto sink = [](void *context, void *data, int size) {
        auto *buf = static_cast<std::vector<std::uint8_t> *>(context);
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        buf->insert(buf->end(), bytes, bytes + size);
    };
    const int stride = static_cast<int>(width) * 4;
    if (stbi_write_png_to_func(sink, &out, static_cast<int>(width), static_cast<int>(height), 4,
                               rgba.data(), stride) == 0) {
        out.clear();
    }
    return out;
}

} // namespace nodehammer::viewer

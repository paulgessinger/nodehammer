#include "png_export_readback.hpp"

// GL GPU→CPU readback for the PNG export, covering both the GLES3 (WebGL2) and
// the desktop GLCORE backends — the readback code is identical, only the GL
// header (and how the entry points are resolved) differs per backend.
//
// sokol exposes the backing GL texture name behind an sg_image via
// sg_gl_query_image_info (tex[] per inflight slot + tex_target). We attach that
// texture to a throwaway framebuffer and glReadPixels it. The read is
// synchronous — glReadPixels blocks until the pixels are available — so begin()
// does all the work and poll() just hands back the result, mirroring the Metal
// path. The exporter only calls begin() a few frames after the capture pass, so
// the source texture already holds finished pixels.
//
// Two GL specifics handled here:
//   * GL framebuffers are bottom-left origin; the readback contract is top-left,
//     so rows are flipped on copy-out.
//   * glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE) returns *logical* RGBA regardless
//     of the color buffer's internal byte order, so there is never a swizzle to
//     do here (unlike Metal/WGPU, which read raw texels). The GLES3 swapchain is
//     RGBA8 anyway.

#if defined(SOKOL_GLES3)
// Emscripten/WebGL2 ships the GLES3 prototypes and links them for us.
#include <GLES3/gl3.h>
#elif defined(SOKOL_GLCORE)
// Desktop GL. Every entry point this TU touches (FBO objects, glReadBuffer,
// glReadPixels, the GL_PACK_* pixel-store state) is core since GL 3.0 and is
// exported directly by the platform's libGL, which the viewer already links
// (see the UNIX branch of nh_add_sokol_lib in cmake/Sokol.cmake). So we don't
// need a dynamic function loader (GLAD/GLEW) — asking <GL/gl.h> for the
// extension prototypes via GL_GLEXT_PROTOTYPES lets the linker bind them
// straight against libGL. This matches how sokol_app.h itself pulls in GL on
// the GLX/EGL desktop paths.
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#else
#error                                                                                             \
    "png_export_readback_gl.cpp compiled without a GL backend define (SOKOL_GLES3 / SOKOL_GLCORE)."
#endif

#include <algorithm>
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
                          sg_pixel_format /*format*/) {
    impl_->status = ReadbackStatus::Failed;
    impl_->pixels.clear();
    if (width == 0 || height == 0) {
        return false;
    }

    sg_gl_image_info info = sg_gl_query_image_info(image);
    const int slot = info.active_slot;
    GLuint tex = (slot >= 0 && slot < SG_NUM_INFLIGHT_FRAMES) ? info.tex[slot] : 0u;
    if (tex == 0u) {
        tex = info.tex[0];
    }
    if (tex == 0u) {
        return false;
    }
    const GLenum target = (info.tex_target != 0u) ? info.tex_target : GL_TEXTURE_2D;

    // Save the GL state we touch so sokol's own state cache stays consistent.
    GLint prev_read_fbo = 0;
    GLint prev_pack_align = 4;
    GLint prev_pack_row_len = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &prev_pack_row_len);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, tex, 0);

    bool ok = (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    std::vector<std::uint8_t> raw;
    if (ok) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        raw.resize(static_cast<std::size_t>(width) * height * 4u);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                     GL_UNSIGNED_BYTE, raw.data());
    }

    // Restore state and drop the scratch FBO.
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
    glPixelStorei(GL_PACK_ROW_LENGTH, prev_pack_row_len);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
    glDeleteFramebuffers(1, &fbo);

    if (!ok) {
        return false;
    }

    // GL is bottom-left origin; flip rows into the top-left contract.
    const std::size_t row = static_cast<std::size_t>(width) * 4u;
    impl_->pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *srow = raw.data() + static_cast<std::size_t>(height - 1u - y) * row;
        std::uint8_t *drow = impl_->pixels.data() + static_cast<std::size_t>(y) * row;
        std::copy(srow, srow + row, drow);
    }

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
    // A normal color attachment can be bound to an FBO and glReadPixels'd as-is,
    // so no special image is needed — let the caller allocate the standard one.
    return sg_image{SG_INVALID_ID};
}

} // namespace nodehammer::viewer

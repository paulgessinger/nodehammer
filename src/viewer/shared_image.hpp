#pragma once

#include <sokol_gfx.h>

#include <memory>

namespace nodehammer::viewer {

/// Reference-counted owner of an `sg_image`. Copies share ownership; the
/// underlying image is destroyed exactly once, via `sg_destroy_image`, when the
/// last `SharedImage` referencing it is dropped.
///
/// This lets several consumers hold the same GPU image without a double-free or
/// a redundant upload — e.g. the two `SceneRenderer`s (base + Boolean cut) can
/// share a single IBL bake instead of baking the environment twice.
///
/// Lifetime caveat: the image is destroyed wherever the final reference goes
/// away, so make sure that last drop happens on the render thread and outside an
/// active `sg_begin_pass`/`sg_end_pass` bracket (sokol's usual destruction rule).
class SharedImage {
  public:
    SharedImage() = default;

    /// Adopt ownership of a freshly created image. An invalid handle
    /// (`SG_INVALID_ID`) produces an empty `SharedImage` with no deleter.
    explicit SharedImage(sg_image img) {
        if (img.id != SG_INVALID_ID) {
            img_ = std::shared_ptr<sg_image>(new sg_image(img), [](sg_image *p) {
                sg_destroy_image(*p);
                delete p;
            });
        }
    }

    /// The raw handle, or a zeroed `sg_image` when empty.
    [[nodiscard]] sg_image get() const { return img_ ? *img_ : sg_image{}; }

    [[nodiscard]] bool valid() const { return static_cast<bool>(img_); }

    /// Drop this reference (destroying the image if it was the last one).
    void reset() { img_.reset(); }

  private:
    std::shared_ptr<sg_image> img_;
};

} // namespace nodehammer::viewer

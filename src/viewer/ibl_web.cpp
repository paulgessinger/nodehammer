#include "ibl.hpp"

#include "ibl_bake_internal.hpp"

#include <sokol_time.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace nodehammer::viewer {

struct IblBakeJob::Impl {
    enum class Stage : uint8_t { Idle, BrdfLut, Irradiance, Prefilter, Done };

    bool started{false};
    Stage stage{Stage::Idle};
    int face{0};
    int mip{0};
    int y{0};
    int x{0};
    IblBakeData partial;

    void bakeOnePixel();
    void advanceIterator();
};

void IblBakeJob::Impl::bakeOnePixel() {
    switch (stage) {
    case Stage::BrdfLut: {
        const size_t idx = (static_cast<size_t>(y) * kBrdfLutSize + static_cast<size_t>(x)) * 4;
        bakeBrdfPixel(x, y, &partial.brdf_lut[idx]);
        break;
    }
    case Stage::Irradiance: {
        const size_t face_off = static_cast<size_t>(face) * irradianceFaceBytes();
        const size_t idx =
            face_off + (static_cast<size_t>(y) * kIrradianceSize + static_cast<size_t>(x)) * 4;
        bakeIrradiancePixel(face, x, y, &partial.irradiance_combined[idx]);
        break;
    }
    case Stage::Prefilter: {
        const int size = std::max(1, kPrefilterSize >> mip);
        const size_t face_off = static_cast<size_t>(face) * prefilterFaceBytes(mip);
        const size_t idx =
            face_off +
            (static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4;
        bakePrefilterPixel(mip, face, x, y, &partial.prefilter_mips[static_cast<size_t>(mip)][idx]);
        break;
    }
    case Stage::Idle:
    case Stage::Done:
        break;
    }
}

void IblBakeJob::Impl::advanceIterator() {
    switch (stage) {
    case Stage::BrdfLut:
        if (++x >= kBrdfLutSize) {
            x = 0;
            if (++y >= kBrdfLutSize) {
                y = 0;
                stage = Stage::Irradiance;
            }
        }
        break;
    case Stage::Irradiance:
        if (++x >= kIrradianceSize) {
            x = 0;
            if (++y >= kIrradianceSize) {
                y = 0;
                if (++face >= 6) {
                    face = 0;
                    stage = Stage::Prefilter;
                }
            }
        }
        break;
    case Stage::Prefilter: {
        const int size = std::max(1, kPrefilterSize >> mip);
        if (++x >= size) {
            x = 0;
            if (++y >= size) {
                y = 0;
                if (++face >= 6) {
                    face = 0;
                    if (++mip >= kPrefilterMips) {
                        mip = 0;
                        stage = Stage::Done;
                    }
                }
            }
        }
        break;
    }
    case Stage::Idle:
    case Stage::Done:
        break;
    }
}

IblBakeJob::IblBakeJob() : impl_(std::make_unique<Impl>()) {}
IblBakeJob::~IblBakeJob() = default;

void IblBakeJob::start() {
    if (impl_->started) {
        return;
    }
    impl_->started = true;
    allocateBakeBuffers(impl_->partial);
    impl_->stage = Impl::Stage::BrdfLut;
    impl_->face = 0;
    impl_->mip = 0;
    impl_->y = 0;
    impl_->x = 0;
}

bool IblBakeJob::advance(uint64_t budget_ns) {
    if (!impl_->started) {
        start();
    }
    if (impl_->stage == Impl::Stage::Done) {
        return true;
    }
    // Check the clock every N pixels rather than on every iteration so the
    // stm_now() / stm_diff() cost doesn't dominate cheap pixels (e.g. small
    // prefilter mips).
    constexpr int kPixelsPerClockCheck = 8;
    const uint64_t start_ticks = stm_now();
    int since_check = 0;
    while (impl_->stage != Impl::Stage::Done) {
        impl_->bakeOnePixel();
        impl_->advanceIterator();
        if (++since_check >= kPixelsPerClockCheck) {
            since_check = 0;
            if (stm_ns(stm_diff(stm_now(), start_ticks)) >= static_cast<double>(budget_ns)) {
                break;
            }
        }
    }
    return impl_->stage == Impl::Stage::Done;
}

double IblBakeJob::progress() const {
    // Weight each stage's pixels by sample count so the bar advances
    // roughly linearly in time. Sample counts: BRDF/irradiance use
    // kBrdfSamples/kIrradianceSamples (1024 each); prefilter uses
    // kPrefilterSamples (256) per pixel.
    constexpr double kBrdfWeightPerPixel = static_cast<double>(kBrdfSamples);
    constexpr double kIrradianceWeightPerPixel = static_cast<double>(kIrradianceSamples);
    constexpr double kPrefilterWeightPerPixel = static_cast<double>(kPrefilterSamples);

    constexpr double brdfTotal =
        static_cast<double>(kBrdfLutSize) * kBrdfLutSize * kBrdfWeightPerPixel;
    constexpr double irradianceTotal =
        static_cast<double>(kIrradianceSize) * kIrradianceSize * 6.0 * kIrradianceWeightPerPixel;
    auto prefilter_size_at = [](int m) { return std::max(1, kPrefilterSize >> m); };
    double prefilterTotal = 0.0;
    for (int m = 0; m < kPrefilterMips; ++m) {
        const double sz = static_cast<double>(prefilter_size_at(m));
        prefilterTotal += sz * sz * 6.0 * kPrefilterWeightPerPixel;
    }
    const double grandTotal = brdfTotal + irradianceTotal + prefilterTotal;

    if (impl_->stage == Impl::Stage::Done) {
        return 1.0;
    }
    if (impl_->stage == Impl::Stage::Idle) {
        return 0.0;
    }

    double done = 0.0;
    if (impl_->stage == Impl::Stage::BrdfLut) {
        const double pixels = static_cast<double>(impl_->y) * kBrdfLutSize + impl_->x;
        done = pixels * kBrdfWeightPerPixel;
    } else if (impl_->stage == Impl::Stage::Irradiance) {
        done = brdfTotal;
        const double pixelsThisFace = static_cast<double>(impl_->y) * kIrradianceSize + impl_->x;
        const double facesDone = static_cast<double>(impl_->face);
        const double pixels = facesDone * kIrradianceSize * kIrradianceSize + pixelsThisFace;
        done += pixels * kIrradianceWeightPerPixel;
    } else { // Prefilter
        done = brdfTotal + irradianceTotal;
        for (int m = 0; m < impl_->mip; ++m) {
            const double sz = static_cast<double>(prefilter_size_at(m));
            done += sz * sz * 6.0 * kPrefilterWeightPerPixel;
        }
        const double sz = static_cast<double>(prefilter_size_at(impl_->mip));
        const double pixelsThisFace = static_cast<double>(impl_->y) * sz + impl_->x;
        const double facesDone = static_cast<double>(impl_->face);
        done += (facesDone * sz * sz + pixelsThisFace) * kPrefilterWeightPerPixel;
    }
    return std::clamp(done / grandTotal, 0.0, 1.0);
}

IblBakeData IblBakeJob::take() { return std::move(impl_->partial); }

} // namespace nodehammer::viewer

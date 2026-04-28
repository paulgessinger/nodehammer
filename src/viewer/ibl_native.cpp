#include "ibl.hpp"

#include "ibl_bake_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace nodehammer::viewer {

struct IblBakeJob::Impl {
    bool started{false};
    std::thread worker;
    std::atomic<bool> done{false};
    std::atomic<double> progress{0.0};
    IblBakeData result;

    void runNativeBake();
};

void IblBakeJob::Impl::runNativeBake() {
    // Same loop layout as `bakeIbl()`, but reports cumulative weighted
    // progress between rows (BRDF) and per-face-per-mip (cubemaps). Pixel
    // weights match the formula used by the web-side `progress()` so a
    // user toggling between platforms sees the same shape of curve.
    constexpr double kBrdfWeightPerPixel = static_cast<double>(kBrdfSamples);
    constexpr double kIrradianceWeightPerPixel = static_cast<double>(kIrradianceSamples);
    constexpr double kPrefilterWeightPerPixel = static_cast<double>(kPrefilterSamples);

    constexpr double brdfTotal =
        static_cast<double>(kBrdfLutSize) * kBrdfLutSize * kBrdfWeightPerPixel;
    constexpr double irradianceTotal =
        static_cast<double>(kIrradianceSize) * kIrradianceSize * 6.0 * kIrradianceWeightPerPixel;
    double prefilterTotal = 0.0;
    for (int m = 0; m < kPrefilterMips; ++m) {
        const double sz = static_cast<double>(std::max(1, kPrefilterSize >> m));
        prefilterTotal += sz * sz * 6.0 * kPrefilterWeightPerPixel;
    }
    const double grandTotal = brdfTotal + irradianceTotal + prefilterTotal;

    double done_pixels = 0.0;
    auto publish = [&]() {
        progress.store(std::clamp(done_pixels / grandTotal, 0.0, 1.0), std::memory_order_relaxed);
    };

    IblBakeData data;
    allocateBakeBuffers(data);

    // BRDF LUT
    for (int y = 0; y < kBrdfLutSize; ++y) {
        for (int x = 0; x < kBrdfLutSize; ++x) {
            const size_t idx = (static_cast<size_t>(y) * kBrdfLutSize + static_cast<size_t>(x)) * 4;
            bakeBrdfPixel(x, y, &data.brdf_lut[idx]);
        }
        done_pixels += kBrdfLutSize * kBrdfWeightPerPixel;
        publish();
    }

    // Irradiance cubemap
    const size_t irr_face_sz = irradianceFaceBytes();
    for (int face = 0; face < 6; ++face) {
        std::byte *face_ptr = &data.irradiance_combined[static_cast<size_t>(face) * irr_face_sz];
        for (int y = 0; y < kIrradianceSize; ++y) {
            for (int x = 0; x < kIrradianceSize; ++x) {
                const size_t idx =
                    (static_cast<size_t>(y) * kIrradianceSize + static_cast<size_t>(x)) * 4;
                bakeIrradiancePixel(face, x, y, &face_ptr[idx]);
            }
        }
        done_pixels +=
            static_cast<double>(kIrradianceSize) * kIrradianceSize * kIrradianceWeightPerPixel;
        publish();
    }

    // Prefilter cubemap (mipped)
    for (int mip = 0; mip < kPrefilterMips; ++mip) {
        const int size = std::max(1, kPrefilterSize >> mip);
        const size_t face_sz = prefilterFaceBytes(mip);
        for (int face = 0; face < 6; ++face) {
            std::byte *face_ptr =
                &data.prefilter_mips[static_cast<size_t>(mip)][static_cast<size_t>(face) * face_sz];
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                                        static_cast<size_t>(x)) *
                                       4;
                    bakePrefilterPixel(mip, face, x, y, &face_ptr[idx]);
                }
            }
            done_pixels += static_cast<double>(size) * size * kPrefilterWeightPerPixel;
            publish();
        }
    }

    result = std::move(data);
    progress.store(1.0, std::memory_order_relaxed);
}

IblBakeJob::IblBakeJob() : impl_(std::make_unique<Impl>()) {}

IblBakeJob::~IblBakeJob() {
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void IblBakeJob::start() {
    if (impl_->started) {
        return;
    }
    impl_->started = true;
    impl_->progress.store(0.0, std::memory_order_relaxed);
    impl_->worker = std::thread([this] {
        impl_->runNativeBake();
        impl_->done.store(true, std::memory_order_release);
    });
}

bool IblBakeJob::advance(uint64_t /*budget_ns*/) {
    if (!impl_->started) {
        start();
    }
    if (impl_->done.load(std::memory_order_acquire)) {
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        return true;
    }
    return false;
}

double IblBakeJob::progress() const { return impl_->progress.load(std::memory_order_relaxed); }

IblBakeData IblBakeJob::take() { return std::move(impl_->result); }

} // namespace nodehammer::viewer

#include "gpu_pass_timer.hpp"

// No-op GPU pass timer for every backend other than D3D11.
//
// D3D11 is the one backend whose real frame stall (sokol_app's Present(), issued
// after the frame callback returns) is invisible to the App's CPU-side submit
// timers, so it's the only one that needs in-engine GPU timestamps. On Metal /
// GL / WebGPU, `present_ms` / `gpu_wait_ms` already surface GPU backpressure, and
// external profilers cover the rest — so this TU just satisfies the interface.

namespace nodehammer::viewer {

struct GpuPassTimer::Impl {
    GpuPassTimings empty;
};

GpuPassTimer::GpuPassTimer() : impl_(std::make_unique<Impl>()) {}
GpuPassTimer::~GpuPassTimer() = default;

bool GpuPassTimer::enabled() const { return false; }
void GpuPassTimer::beginFrame() {}
void GpuPassTimer::stamp(const char * /*label*/) {}
void GpuPassTimer::endFrame() {}
const GpuPassTimings &GpuPassTimer::results() const { return impl_->empty; }

} // namespace nodehammer::viewer

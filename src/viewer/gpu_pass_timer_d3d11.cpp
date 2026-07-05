#include "gpu_pass_timer.hpp"

// D3D11 per-pass GPU timing via timestamp queries.
//
// sokol exposes the immediate device/context (sg_d3d11_device /
// sg_d3d11_device_context), and sokol_gfx's D3D11 backend records into that
// immediate context in submission order — so issuing our own timestamp End()s
// between sokol's sg_end_pass() calls lands them at the right point on the GPU
// timeline without touching any pipeline state.
//
// A frame's queries can't be read back the same frame without stalling the CPU
// on the GPU, so each frame's query set lives in a ring; we harvest a slot only
// when we come back around to reuse it (kRing frames later), by which point the
// GPU has long finished it. The read uses DONOTFLUSH and simply drops the sample
// if it somehow isn't ready — a missing sample is fine, a CPU stall is not.
//
// Per frame: Begin(disjoint) + End(ts[0]) at beginFrame(); End(ts[i]) at each
// stamp(); End(disjoint) at endFrame(). Segment i spans ts[i]..ts[i+1] and is
// labeled by the stamp that closed it. Duration = (t1 - t0) / disjoint.Frequency.

#include <sokol_gfx.h>

#include <d3d11.h>

#include <cstdio>
#include <cstring>

namespace nodehammer::viewer {

namespace {
constexpr int kRing = 4;                             // frames of queries in flight
constexpr int kTimestamps = kGpuPassMaxSegments + 1; // +1 for the frame-start marker

struct FrameSet {
    ID3D11Query *disjoint{nullptr};
    ID3D11Query *ts[kTimestamps]{};
    char labels[kTimestamps][24]{};
    int used{0}; // number of timestamps recorded (>=1 while recording)
    bool recording{false};
    bool pending{false}; // End(disjoint) issued, awaiting harvest
};
} // namespace

struct GpuPassTimer::Impl {
    ID3D11Device *dev{nullptr};
    ID3D11DeviceContext *ctx{nullptr};
    bool init_tried{false};
    bool enabled{false};

    FrameSet ring[kRing];
    int cur{-1};
    bool frame_active{false};

    GpuPassTimings last;

    void ensureInit() {
        if (init_tried) {
            return;
        }
        init_tried = true;
        dev = static_cast<ID3D11Device *>(const_cast<void *>(sg_d3d11_device()));
        ctx = static_cast<ID3D11DeviceContext *>(const_cast<void *>(sg_d3d11_device_context()));
        enabled = (dev != nullptr) && (ctx != nullptr);
    }

    bool ensureQueries(FrameSet &f) {
        if (f.disjoint == nullptr) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(dev->CreateQuery(&qd, &f.disjoint)) || f.disjoint == nullptr) {
                return false;
            }
        }
        for (auto &q : f.ts) {
            if (q == nullptr) {
                D3D11_QUERY_DESC qd{};
                qd.Query = D3D11_QUERY_TIMESTAMP;
                if (FAILED(dev->CreateQuery(&qd, &q)) || q == nullptr) {
                    return false;
                }
            }
        }
        return true;
    }

    void harvest(FrameSet &f) {
        f.pending = false;
        constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(f.disjoint, &dj, sizeof(dj), kNoFlush) != S_OK) {
            return; // not ready yet — drop this sample rather than stall
        }
        if (dj.Disjoint != FALSE || dj.Frequency == 0) {
            return; // clocks shifted mid-frame — timings unreliable
        }

        UINT64 stamps[kTimestamps]{};
        for (int i = 0; i < f.used; ++i) {
            if (ctx->GetData(f.ts[i], &stamps[i], sizeof(UINT64), kNoFlush) != S_OK) {
                return;
            }
        }

        GpuPassTimings r;
        const int segs = f.used - 1;
        r.count = segs > kGpuPassMaxSegments ? kGpuPassMaxSegments : segs;
        const double inv_freq_ms = 1000.0 / static_cast<double>(dj.Frequency);
        for (int s = 0; s < r.count; ++s) {
            const UINT64 dt = stamps[s + 1] - stamps[s];
            r.segments[s].ms = static_cast<double>(dt) * inv_freq_ms;
            std::memcpy(r.segments[s].label, f.labels[s + 1], sizeof(r.segments[s].label));
            r.segments[s].label[sizeof(r.segments[s].label) - 1] = '\0';
        }
        if (f.used >= 2) {
            r.total_ms = static_cast<double>(stamps[f.used - 1] - stamps[0]) * inv_freq_ms;
        }
        r.valid = true;
        last = r;
    }
};

GpuPassTimer::GpuPassTimer() : impl_(std::make_unique<Impl>()) {}

GpuPassTimer::~GpuPassTimer() {
    for (auto &f : impl_->ring) {
        if (f.disjoint != nullptr) {
            f.disjoint->Release();
        }
        for (auto *q : f.ts) {
            if (q != nullptr) {
                q->Release();
            }
        }
    }
}

bool GpuPassTimer::enabled() const {
    impl_->ensureInit();
    return impl_->enabled;
}

void GpuPassTimer::beginFrame() {
    impl_->ensureInit();
    if (!impl_->enabled) {
        return;
    }
    impl_->cur = (impl_->cur + 1) % kRing;
    FrameSet &f = impl_->ring[impl_->cur];
    if (!impl_->ensureQueries(f)) {
        impl_->enabled = false; // query allocation failed — give up cleanly
        return;
    }
    if (f.pending) {
        impl_->harvest(f);
    }
    impl_->ctx->Begin(f.disjoint);
    impl_->ctx->End(f.ts[0]);
    std::memset(f.labels[0], 0, sizeof(f.labels[0]));
    f.used = 1;
    f.recording = true;
    impl_->frame_active = true;
}

void GpuPassTimer::stamp(const char *label) {
    if (!impl_->frame_active) {
        return;
    }
    FrameSet &f = impl_->ring[impl_->cur];
    if (!f.recording || f.used >= kTimestamps) {
        return; // segment cap reached — drop extra stamps
    }
    impl_->ctx->End(f.ts[f.used]);
    std::snprintf(f.labels[f.used], sizeof(f.labels[f.used]), "%s", label != nullptr ? label : "");
    ++f.used;
}

void GpuPassTimer::endFrame() {
    if (!impl_->frame_active) {
        return;
    }
    FrameSet &f = impl_->ring[impl_->cur];
    impl_->ctx->End(f.disjoint);
    f.recording = false;
    f.pending = true;
    impl_->frame_active = false;
}

const GpuPassTimings &GpuPassTimer::results() const { return impl_->last; }

} // namespace nodehammer::viewer

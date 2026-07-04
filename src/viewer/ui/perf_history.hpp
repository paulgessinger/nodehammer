#pragma once

#include <imgui.h> // ImVec2, ImVector

namespace nodehammer::viewer::ui {

/// Fixed-capacity ring buffer of (time, value) samples for live ImPlot graphs.
/// Mirrors the ScrollingBuffer pattern from the ImPlot demo: once full, new
/// samples overwrite the oldest in place and `offset` records where the logical
/// start is, so the plotted line stays contiguous in time across the wrap.
struct ScrollingBuffer {
    int max_size;
    int offset;
    ImVector<ImVec2> data;

    explicit ScrollingBuffer(int size = 1200) : max_size(size), offset(0) {
        data.reserve(max_size);
    }

    void add(float x, float y) {
        if (data.size() < max_size) {
            data.push_back(ImVec2(x, y));
        } else {
            data[offset] = ImVec2(x, y);
            offset = (offset + 1) % max_size;
        }
    }
};

/// Rolling per-frame timing history backing the Debug panel's perf graphs. The
/// App pushes one sample per rendered frame; the panel reads it back to draw the
/// scrolling line plots alongside the current numeric values.
struct PerfHistory {
    /// Seconds since the first sample, monotonically increasing. Used as the
    /// shared x-axis for every series so they line up in the plot.
    float t{0.f};

    ScrollingBuffer frame_ms;
    // The "CPU submit" total split into its two halves (see App::Impl): `encode`
    // is the CPU time spent encoding the offscreen passes; `present` is the
    // swapchain pass — drawable acquire + composite + commit — which absorbs GPU
    // backpressure. present >> encode ⇒ GPU-bound, not submission-bound.
    ScrollingBuffer encode_ms;
    ScrollingBuffer present_ms;
    // Subset of encode: the in-flight-frames semaphore wait at the first
    // begin_pass. This is GPU backpressure, so the gap between encode and
    // gpu_wait is the true CPU command-encoding cost.
    ScrollingBuffer gpu_wait_ms;
    ScrollingBuffer scene_submit_ms;
    // Real GPU frame time (sum of the per-pass timestamp spans). Distinct from
    // the CPU timers above: on D3D11 the frame's stall lives in sokol_app's
    // Present() — after sg_commit() — so it never shows up in encode/present, and
    // this is the only series that reflects it. Zero on backends without
    // timestamp queries.
    ScrollingBuffer gpu_total_ms;
    ScrollingBuffer fps;

    void push(double dt_seconds, double frame_interval_ms, double encode_ms_value,
              double present_ms_value, double gpu_wait_ms_value, double scene_submit_ms_value,
              double gpu_total_ms_value, float fps_value) {
        t += static_cast<float>(dt_seconds);
        frame_ms.add(t, static_cast<float>(frame_interval_ms));
        encode_ms.add(t, static_cast<float>(encode_ms_value));
        present_ms.add(t, static_cast<float>(present_ms_value));
        gpu_wait_ms.add(t, static_cast<float>(gpu_wait_ms_value));
        scene_submit_ms.add(t, static_cast<float>(scene_submit_ms_value));
        gpu_total_ms.add(t, static_cast<float>(gpu_total_ms_value));
        fps.add(t, fps_value);
    }
};

} // namespace nodehammer::viewer::ui

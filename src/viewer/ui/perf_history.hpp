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
    ScrollingBuffer cpu_submit_ms;
    ScrollingBuffer scene_submit_ms;
    ScrollingBuffer fps;

    void push(double dt_seconds, double frame_interval_ms, double render_submit_ms,
              double scene_submit_ms_value, float fps_value) {
        t += static_cast<float>(dt_seconds);
        frame_ms.add(t, static_cast<float>(frame_interval_ms));
        cpu_submit_ms.add(t, static_cast<float>(render_submit_ms));
        scene_submit_ms.add(t, static_cast<float>(scene_submit_ms_value));
        fps.add(t, fps_value);
    }
};

} // namespace nodehammer::viewer::ui

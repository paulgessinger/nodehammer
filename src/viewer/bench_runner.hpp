#pragma once

#include "gpu_pass_timer.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nodehammer::viewer {

struct Camera;
struct Config;
struct RenderQualitySettings;

/// Draw-submission counts for a measured segment (copied out of the scene
/// renderer's per-frame stats — constant while the camera/state hold).
struct BenchSceneStats {
    std::uint32_t draws{0};
    std::uint32_t instances{0};
    std::uint64_t triangles{0};
};

/// State the App hands the runner each frame. All values reflect the *previous*
/// rendered frame (the GPU timers and stats lag a frame by construction), which
/// is exactly what a measurement window wants to accumulate.
struct BenchFrameInput {
    bool settled{false}; ///< scene uploaded + no build/bake/upload in flight
    bool has_bounds{false};
    glm::vec3 bounds_min{0.f};
    glm::vec3 bounds_max{0.f};
    GpuPassTimings gpu{}; ///< last frame's per-pass GPU times
    BenchSceneStats stats{};
    double frame_ms{0.0};
    double cpu_submit_ms{0.0}; ///< encode + present (CPU submit total)
    bool capture_done{false};  ///< a requested screenshot finished writing
};

/// Actions the runner asks the App to perform this frame.
struct BenchFrameOutput {
    bool request_capture{false}; ///< composite the current frame + read back to `capture_path`
    std::string capture_path;
    bool finished{false}; ///< the whole sequence is done — write JSON + quit
};

/// Drives a fixed, hard-coded benchmark sequence over the viewer: settle, then
/// (cut on) hold / orbit / zoom, then (cut off) orbit / zoom — measuring per-pass
/// GPU time over each window and grabbing a screenshot of the converged frame.
/// Pure sequence + aggregation logic: it mutates the camera / config / quality it
/// was handed and reads back timings through BenchFrameInput; every GPU-touching
/// action (settle query, capture, quit) is the App's job via BenchFrameOutput.
///
/// The whole scenario lives in the constructor (steps_) on purpose — per the
/// design, we hard-code the sequence rather than build a config format for it.
class BenchRunner {
  public:
    BenchRunner(Camera &camera, Config &cfg, RenderQualitySettings &quality,
                std::string json_out_path, std::string scene_label, std::string temp_dir);

    /// One frame of the scenario. Call before render(): it applies this frame's
    /// camera/state, accumulates the previous frame's timings when inside a
    /// measure window, and returns capture/quit requests.
    BenchFrameOutput update(const BenchFrameInput &in);

    /// Emit the accumulated results as JSON — to `json_out_path` and to stdout.
    /// Call once, after update() reports `finished`.
    void writeResults(const char *backend_name, std::uint32_t width, std::uint32_t height) const;

  private:
    struct Step {
        enum class Kind { AwaitSettle, SetCut, FrameCamera, Measure };
        Kind kind;
        bool cut_on{true};              // SetCut
        float yaw_deg{0.f};             // FrameCamera
        float pitch_deg{0.f};           // FrameCamera
        float zoom{1.f};                // FrameCamera: distance = framed × zoom (<1 = closer)
        std::string label;              // Measure
        float orbit_deg_per_frame{0.f}; // Measure: 0 = static hold
        int warmup{0};                  // Measure
        int frames{0};                  // Measure
        bool capture{false};            // Measure
    };

    struct Stat {
        double median{0.0};
        double p95{0.0};
        double min{0.0};
        double max{0.0};
        int n{0};
    };

    struct Accum {
        std::vector<double> total, scene, ao, denoise, composite, cpu, frame;
        BenchSceneStats stats{};
        bool gpu_valid{true};
        void clear() {
            total.clear();
            scene.clear();
            ao.clear();
            denoise.clear();
            composite.clear();
            cpu.clear();
            frame.clear();
            stats = {};
            gpu_valid = true;
        }
    };

    struct SegResult {
        std::string label;
        int frames{0};
        bool gpu_valid{false};
        Stat total, scene, ao, denoise, composite, cpu, frame;
        BenchSceneStats stats{};
        std::string screenshot;
    };

    void applyFrameCamera(const BenchFrameInput &in, const Step &s);
    BenchFrameOutput updateMeasure(const BenchFrameInput &in, const Step &s);
    void accumulate(const BenchFrameInput &in);
    void finalizeSegment(const Step &s, std::string screenshot);
    void advanceStep();
    static Stat computeStat(const std::vector<double> &v);

    Camera &camera_;
    Config &cfg_;
    RenderQualitySettings &quality_;
    std::string json_path_;
    std::string scene_label_;
    std::string temp_dir_;

    std::vector<Step> steps_;
    std::size_t step_{0};
    int frame_in_step_{0};
    int settle_confirm_{0};
    bool capture_requested_{false};
    std::string pending_shot_;
    Accum accum_;
    std::vector<SegResult> results_;
};

} // namespace nodehammer::viewer

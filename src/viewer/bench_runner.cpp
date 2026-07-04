#include "bench_runner.hpp"

#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/config.hpp>
#include <nodehammer/viewer/render_quality.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <print>
#include <string>
#include <vector>

namespace nodehammer::viewer {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.f;

// SetCut: after toggling the cut, give the build controller a few frames to
// notice and kick the rebuild, then require the scene to read "settled" for a
// stretch so we know the retessellation finished before measuring.
constexpr int kSetCutMinWait = 6;
constexpr int kSettleConfirm = 8;

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto n = v.size();
    auto idx = static_cast<std::size_t>(p * static_cast<double>(n - 1) + 0.5);
    if (idx >= n) {
        idx = n - 1;
    }
    return v[idx];
}

// GPU ms of a named pass in a frame's timing set, or 0 if that pass didn't run.
double segMs(const GpuPassTimings &g, const char *label) {
    for (int i = 0; i < g.count; ++i) {
        if (std::strcmp(g.segments[i].label, label) == 0) {
            return g.segments[i].ms;
        }
    }
    return 0.0;
}

std::string escapeJson(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace

BenchRunner::BenchRunner(Camera &camera, Config &cfg, RenderQualitySettings &quality,
                         std::string json_out_path, std::string scene_label, std::string temp_dir)
    : camera_(camera), cfg_(cfg), quality_(quality), json_path_(std::move(json_out_path)),
      scene_label_(std::move(scene_label)), temp_dir_(std::move(temp_dir)) {
    using K = Step::Kind;
    // The scenario. Cut is already ON at startup (the App forces it before the
    // first build), so we open by settling, run the cut-on lap, then flip the
    // cut off and run the same orbit/zoom lap for the A/B.
    const auto measure = [](std::string label, float dpf, bool capture) {
        Step s;
        s.kind = K::Measure;
        s.label = std::move(label);
        s.orbit_deg_per_frame = dpf;
        s.warmup = 30;
        s.frames = dpf != 0.f ? 360 : 90; // a full turn while orbiting; shorter for a hold
        s.capture = capture;
        return s;
    };
    const auto frame_cam = [](float yaw, float pitch, float zoom) {
        Step s;
        s.kind = K::FrameCamera;
        s.yaw_deg = yaw;
        s.pitch_deg = pitch;
        s.zoom = zoom;
        return s;
    };
    const auto set_cut = [](bool on) {
        Step s;
        s.kind = K::SetCut;
        s.cut_on = on;
        return s;
    };

    steps_.push_back({K::AwaitSettle});
    // Cut ON lap.
    steps_.push_back(frame_cam(30.f, 20.f, 1.0f));
    steps_.push_back(measure("hold_cut_on", 0.f, true));
    steps_.push_back(frame_cam(0.f, 20.f, 1.0f));
    steps_.push_back(measure("orbit_cut_on", 1.0f, true));
    steps_.push_back(frame_cam(20.f, 15.f, 0.5f)); // zoom in so the scene mostly fills the frame
    steps_.push_back(measure("zoom_cut_on", 0.f, true));
    // Cut OFF lap (the overdraw A/B).
    steps_.push_back(set_cut(false));
    steps_.push_back(frame_cam(0.f, 20.f, 1.0f));
    steps_.push_back(measure("orbit_cut_off", 1.0f, true));
    steps_.push_back(frame_cam(20.f, 15.f, 0.5f));
    steps_.push_back(measure("zoom_cut_off", 0.f, true));
}

BenchRunner::Stat BenchRunner::computeStat(const std::vector<double> &v) {
    Stat s;
    s.n = static_cast<int>(v.size());
    if (v.empty()) {
        return s;
    }
    s.median = percentile(v, 0.5);
    s.p95 = percentile(v, 0.95);
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    return s;
}

void BenchRunner::advanceStep() {
    ++step_;
    frame_in_step_ = 0;
    settle_confirm_ = 0;
    capture_requested_ = false;
    pending_shot_.clear();
    accum_.clear();
}

void BenchRunner::applyFrameCamera(const BenchFrameInput &in, const Step &s) {
    if (in.has_bounds) {
        // Frame the *base* bounds (passed in regardless of cut state) so the pose
        // is identical across the cut-on/cut-off laps — the A/B stays honest.
        camera_.frameBounds(in.bounds_min, in.bounds_max);
    }
    camera_.projection = ProjectionMode::Perspective;
    camera_.yaw = s.yaw_deg * kDegToRad;
    camera_.pitch = s.pitch_deg * kDegToRad;
    camera_.distance *= s.zoom;
}

void BenchRunner::accumulate(const BenchFrameInput &in) {
    if (!in.gpu.valid) {
        accum_.gpu_valid = false;
    }
    accum_.total.push_back(in.gpu.total_ms);
    accum_.scene.push_back(segMs(in.gpu, "scene"));
    accum_.ao.push_back(segMs(in.gpu, "ao"));
    accum_.denoise.push_back(segMs(in.gpu, "denoise"));
    accum_.composite.push_back(segMs(in.gpu, "composite"));
    accum_.cpu.push_back(in.cpu_submit_ms);
    accum_.frame.push_back(in.frame_ms);
    accum_.stats = in.stats; // constant while the state holds — last sample wins
}

void BenchRunner::finalizeSegment(const Step &s, std::string screenshot) {
    SegResult r;
    r.label = s.label;
    r.frames = static_cast<int>(accum_.total.size());
    r.gpu_valid = accum_.gpu_valid && !accum_.total.empty();
    r.total = computeStat(accum_.total);
    r.scene = computeStat(accum_.scene);
    r.ao = computeStat(accum_.ao);
    r.denoise = computeStat(accum_.denoise);
    r.composite = computeStat(accum_.composite);
    r.cpu = computeStat(accum_.cpu);
    r.frame = computeStat(accum_.frame);
    r.stats = accum_.stats;
    r.screenshot = std::move(screenshot);
    results_.push_back(std::move(r));
    std::println("viewer: bench segment '{}' done — GPU {:.2f} ms (scene {:.2f}), {} frames",
                 s.label, r.total.median, r.scene.median, r.frames);
}

BenchFrameOutput BenchRunner::updateMeasure(const BenchFrameInput &in, const Step &s) {
    BenchFrameOutput out;
    // Warmup: drive the camera but don't record (AO temporal history + any target
    // realloc need to converge first).
    if (frame_in_step_ < s.warmup) {
        if (s.orbit_deg_per_frame != 0.f) {
            camera_.orbit(s.orbit_deg_per_frame * kDegToRad, 0.f);
        }
        ++frame_in_step_;
        return out;
    }
    const int m = frame_in_step_ - s.warmup;
    if (m < s.frames) {
        accumulate(in); // record last frame's timings, then advance the camera
        if (s.orbit_deg_per_frame != 0.f) {
            camera_.orbit(s.orbit_deg_per_frame * kDegToRad, 0.f);
        }
        ++frame_in_step_;
        return out;
    }
    // Measurement window done — grab the converged frame, then finalize.
    if (s.capture) {
        if (!capture_requested_) {
            const std::filesystem::path p =
                std::filesystem::path(temp_dir_) / ("nh_bench_" + s.label + ".png");
            pending_shot_ = p.string();
            out.request_capture = true;
            out.capture_path = pending_shot_;
            capture_requested_ = true;
            return out;
        }
        if (!in.capture_done) {
            return out; // wait for the PNG to be written
        }
    }
    finalizeSegment(s, capture_requested_ ? pending_shot_ : std::string{});
    advanceStep();
    return out;
}

BenchFrameOutput BenchRunner::update(const BenchFrameInput &in) {
    BenchFrameOutput out;
    if (step_ >= steps_.size()) {
        out.finished = true;
        return out;
    }
    const Step &s = steps_[step_];
    switch (s.kind) {
    case Step::Kind::AwaitSettle:
        if (in.settled) {
            advanceStep();
        }
        return out;

    case Step::Kind::SetCut:
        if (frame_in_step_ == 0) {
            cfg_.boolean_cut = s.cut_on;
            settle_confirm_ = 0;
        }
        ++frame_in_step_;
        if (frame_in_step_ < kSetCutMinWait) {
            return out; // let the build controller notice the toggle first
        }
        if (in.settled) {
            if (++settle_confirm_ >= kSettleConfirm) {
                advanceStep();
            }
        } else {
            settle_confirm_ = 0;
        }
        return out;

    case Step::Kind::FrameCamera:
        applyFrameCamera(in, s);
        advanceStep();
        return out;

    case Step::Kind::Measure:
        return updateMeasure(in, s);
    }
    return out;
}

void BenchRunner::writeResults(const char *backend_name, std::uint32_t width,
                               std::uint32_t height) const {
    const auto emit_stat = [](std::string &o, const char *key, const BenchRunner::Stat &st) {
        std::string buf;
        buf += "\"";
        buf += key;
        buf += "\":{\"median\":" + std::format("{:.4f}", st.median) +
               ",\"p95\":" + std::format("{:.4f}", st.p95) +
               ",\"min\":" + std::format("{:.4f}", st.min) +
               ",\"max\":" + std::format("{:.4f}", st.max) + ",\"n\":" + std::to_string(st.n) + "}";
        o += buf;
    };

    std::string j;
    j += "{\n";
    j += "  \"scene\": \"" + escapeJson(scene_label_) + "\",\n";
    j += "  \"backend\": \"" + escapeJson(backend_name) + "\",\n";
    j += "  \"resolution\": [" + std::to_string(width) + ", " + std::to_string(height) + "],\n";
    j += "  \"segments\": [\n";
    for (std::size_t i = 0; i < results_.size(); ++i) {
        const SegResult &r = results_[i];
        j += "    {\n";
        j += "      \"label\": \"" + escapeJson(r.label) + "\",\n";
        j += "      \"frames\": " + std::to_string(r.frames) + ",\n";
        j += "      \"gpu_valid\": " + std::string(r.gpu_valid ? "true" : "false") + ",\n";
        j += "      \"gpu_ms\": {";
        emit_stat(j, "total", r.total);
        j += ", ";
        emit_stat(j, "scene", r.scene);
        j += ", ";
        emit_stat(j, "ao", r.ao);
        j += ", ";
        emit_stat(j, "denoise", r.denoise);
        j += ", ";
        emit_stat(j, "composite", r.composite);
        j += "},\n";
        j += "      \"cpu_submit_ms\": {";
        emit_stat(j, "v", r.cpu);
        j += "},\n";
        j += "      \"frame_ms\": {";
        emit_stat(j, "v", r.frame);
        j += "},\n";
        j += "      \"draws\": " + std::to_string(r.stats.draws) +
             ", \"instances\": " + std::to_string(r.stats.instances) +
             ", \"triangles\": " + std::to_string(r.stats.triangles) + ",\n";
        j += "      \"screenshot\": \"" + escapeJson(r.screenshot) + "\"\n";
        j += i + 1 < results_.size() ? "    },\n" : "    }\n";
    }
    j += "  ]\n";
    j += "}\n";

    std::println("{}", j);
    if (!json_path_.empty()) {
        std::ofstream out{json_path_, std::ios::binary | std::ios::trunc};
        if (out) {
            out.write(j.data(), static_cast<std::streamsize>(j.size()));
        }
        if (out) {
            std::println("viewer: bench results written to {}", json_path_);
        } else {
            std::println(stderr, "viewer: failed to write bench results to {}", json_path_);
        }
    }
}

} // namespace nodehammer::viewer

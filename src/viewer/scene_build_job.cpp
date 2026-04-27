#include "scene_build_job.hpp"

#include <nodehammer/scene_build.hpp>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace nodehammer::viewer {

namespace {

void log_pre_build(const std::string &config_path, const std::string &input_path) {
    std::error_code ec;
    const bool config_exists = std::filesystem::exists(config_path, ec);
    const auto config_size = config_exists ? std::filesystem::file_size(config_path, ec) : 0;
    const bool input_exists = std::filesystem::exists(input_path, ec);
    const auto input_size = input_exists ? std::filesystem::file_size(input_path, ec) : 0;
    std::fprintf(stderr,
                 "scene_build_job: about to build — config=%s (exists=%d size=%zu) "
                 "input=%s (exists=%d size=%zu)\n",
                 config_path.c_str(), config_exists ? 1 : 0, static_cast<size_t>(config_size),
                 input_path.c_str(), input_exists ? 1 : 0, static_cast<size_t>(input_size));
    if (!config_exists || !input_exists) {
        // List the offending parent directory so we can see what's actually there.
        for (const auto &p : {config_path, input_path}) {
            const auto parent = std::filesystem::path(p).parent_path();
            std::fprintf(stderr, "scene_build_job:   listing %s:\n", parent.string().c_str());
            for (auto it = std::filesystem::directory_iterator(parent, ec);
                 !ec && it != std::filesystem::directory_iterator(); ++it) {
                std::fprintf(stderr, "scene_build_job:     %s\n", it->path().string().c_str());
            }
            if (ec) {
                std::fprintf(stderr, "scene_build_job:     (iteration error: %s)\n",
                             ec.message().c_str());
                ec.clear();
            }
        }
    }
}

} // namespace

SceneBuildJob::~SceneBuildJob() {
#ifndef __EMSCRIPTEN__
    if (worker_.joinable()) {
        worker_.join();
    }
#endif
}

void SceneBuildJob::start(std::string config_path, std::string input_path) {
    config_path_ = std::move(config_path);
    input_path_ = std::move(input_path);
    result_ = {};

#ifdef __EMSCRIPTEN__
    // Web: defer the synchronous build by one poll so the caller's UI gets
    // a chance to paint a "Tessellating…" frame before the page freezes.
    state_ = State::Queued;
#else
    done_.store(false, std::memory_order_release);
    state_ = State::Running;
    log_pre_build(config_path_, input_path_);
    worker_ = std::thread([this] {
        SceneBuildResult res = build_scene_from_paths(config_path_, input_path_);
        result_ = std::move(res);
        done_.store(true, std::memory_order_release);
    });
#endif
}

bool SceneBuildJob::poll() {
#ifdef __EMSCRIPTEN__
    switch (state_) {
    case State::Idle:
    case State::Done:
        return state_ == State::Done;
    case State::Queued:
        // Burn one poll so the caller can repaint, then run on the next.
        state_ = State::Running;
        return false;
    case State::Running:
        log_pre_build(config_path_, input_path_);
        result_ = build_scene_from_paths(config_path_, input_path_);
        state_ = State::Done;
        return true;
    }
    return false;
#else
    if (state_ == State::Done) {
        return true;
    }
    if (state_ != State::Running) {
        return false;
    }
    if (!done_.load(std::memory_order_acquire)) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    state_ = State::Done;
    return true;
#endif
}

SceneBuildResult SceneBuildJob::take() {
    SceneBuildResult out = std::move(result_);
    result_ = {};
    state_ = State::Idle;
    config_path_.clear();
    input_path_.clear();
    return out;
}

} // namespace nodehammer::viewer

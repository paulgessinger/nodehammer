#include "scene_build_job.hpp"

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <cstdio>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>

namespace nodehammer::viewer {

namespace {

void logPreBuild(const std::string &config_path, const std::string &input_path) {
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

    prep_ = {};
    tess_job_ = ::nodehammer::TessellationJob{};

#ifdef __EMSCRIPTEN__
    // Web: defer all real work by one poll so the caller can paint a
    // "Tessellating…" frame before the upstream stages run.
    state_ = State::Queued;
#else
    done_.store(false, std::memory_order_release);
    state_ = State::Running;
    logPreBuild(config_path_, input_path_);
    worker_ = std::thread([this] {
        prep_ = ::nodehammer::prepareSceneForTessellation(config_path_, input_path_);
        if (!prep_.ok) {
            result_.scene = nullptr;
            result_.diags = std::move(prep_.diags);
            done_.store(true, std::memory_order_release);
            return;
        }
        tess_job_.start(prep_.config, prep_.scene);
        // Run the iterator straight through — the thread isn't budget-
        // driven, but `totalNodes` / `processedNodes` get updated as we
        // go, so the main-thread UI can show a real-time bar.
        while (!tess_job_.advance(std::numeric_limits<uint64_t>::max())) {
        }
        ::nodehammer::TessellationPassResult tess = tess_job_.take();
        prep_.diags.append(tess.diags);
        if (tess.diags.hasErrors()) {
            result_.scene = nullptr;
        } else {
            result_.scene = std::make_shared<::nodehammer::RenderScene>(std::move(tess.scene));
        }
        result_.diags = std::move(prep_.diags);
        done_.store(true, std::memory_order_release);
    });
#endif
}

bool SceneBuildJob::poll(uint64_t budget_ns) {
#ifdef __EMSCRIPTEN__
    switch (state_) {
    case State::Idle:
        return false;
    case State::Done:
        return true;
    case State::Queued:
        // Burn one poll so the caller's last frame paints before we run
        // the synchronous prep + first tessellation slice.
        state_ = State::PrepPending;
        return false;
    case State::PrepPending: {
        logPreBuild(config_path_, input_path_);
        prep_ = ::nodehammer::prepareSceneForTessellation(config_path_, input_path_);
        if (!prep_.ok) {
            // Upstream stage failed — package the diags and finish.
            result_.scene = nullptr;
            result_.diags = std::move(prep_.diags);
            state_ = State::Done;
            return true;
        }
        tess_job_.start(prep_.config, prep_.scene);
        state_ = State::Tessellating;
        // Run a first slice immediately so we make visible progress on
        // this poll — but stop within the budget so we yield to render.
        if (tess_job_.advance(budget_ns)) {
            state_ = State::Finalizing;
        }
        return false;
    }
    case State::Tessellating:
        if (tess_job_.advance(budget_ns)) {
            state_ = State::Finalizing;
        }
        return false;
    case State::Finalizing: {
        ::nodehammer::TessellationPassResult tess = tess_job_.take();
        prep_.diags.append(tess.diags);
        if (tess.diags.hasErrors()) {
            result_.scene = nullptr;
        } else {
            result_.scene = std::make_shared<::nodehammer::RenderScene>(std::move(tess.scene));
        }
        result_.diags = std::move(prep_.diags);
        prep_ = {};
        state_ = State::Done;
        return true;
    }
    }
    return false;
#else
    static_cast<void>(budget_ns);
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
    prep_ = {};
    tess_job_ = ::nodehammer::TessellationJob{};
    return out;
}

size_t SceneBuildJob::tessellationTotal() const { return tess_job_.totalNodes(); }
size_t SceneBuildJob::tessellationProcessed() const { return tess_job_.processedNodes(); }

SceneBuildJob::Phase SceneBuildJob::phase() const {
#ifdef __EMSCRIPTEN__
    switch (state_) {
    case State::Idle:
        return Phase::Idle;
    case State::Queued:
    case State::PrepPending:
        return Phase::Preparing;
    case State::Tessellating:
        return Phase::Tessellating;
    case State::Finalizing:
        return Phase::Finalizing;
    case State::Done:
        return Phase::Done;
    }
    return Phase::Idle;
#else
    switch (state_) {
    case State::Idle:
        return Phase::Idle;
    case State::Running:
        // Native runs the whole pipeline on a worker thread — collapse
        // it under "Tessellating" since that's by far the longest stage
        // and the UI doesn't have visibility into the thread's progress.
        return Phase::Tessellating;
    case State::Done:
        return Phase::Done;
    }
    return Phase::Idle;
#endif
}

} // namespace nodehammer::viewer

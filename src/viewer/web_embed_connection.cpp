#include <viewer/viewer_protocol.hpp>
#include <viewer/web_embed_connection.hpp>

#include <emscripten/emscripten.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <print>

// Embed mode (docs/viewer-embed.md): the viewer runs inside an iframe and talks
// to the embedding page over window.postMessage. Both directions are inert when
// the page is not framed (window.parent === window).
//
// JSON envelopes cross this boundary unchanged; the shared protocol codec owns
// message names and validation. The adapter only implements delivery policy.
// clang-format off
EM_JS(void, nh_viewer_embed_install, (), {
    if (Module['__nhEmbedInstalled']) {
        return;
    }
    Module['__nhEmbedInstalled'] = true;
    if (window.parent === window) {
        return;
    }
    Module['__nhEmbedListener'] = function(ev) {
        var data = ev.data;
        if (!data || typeof data !== 'object' || typeof data.type !== 'string') {
            return;
        }
        var ptr = 0;
        try {
            var json = JSON.stringify(data);
            var len = lengthBytesUTF8(json) + 1;
            ptr = Module['_malloc'](len);
            stringToUTF8(json, ptr, len);
            Module['_nh_viewer_embed_command'](ptr);
        } catch (e) {
            console.warn('nodehammer: invalid command payload', e);
        } finally {
            if (ptr) { Module['_free'](ptr); }
        }
    };
    window.addEventListener('message', Module['__nhEmbedListener']);
});

// Viewer → host: the codec has already built the complete event envelope.
EM_JS(void, nh_viewer_embed_post, (const char *event_json), {
    if (window.parent !== window) {
        window.parent.postMessage(JSON.parse(UTF8ToString(event_json)), '*');
    }
});
EM_JS(void, nh_viewer_embed_uninstall, (), {
    if (Module['__nhEmbedListener']) {
        window.removeEventListener('message', Module['__nhEmbedListener']);
        delete Module['__nhEmbedListener'];
    }
    Module['__nhEmbedInstalled'] = false;
});
// clang-format on

namespace nodehammer::viewer {
namespace {
std::weak_ptr<ViewerConnection> active_connection;

class WebEmbedConnection final : public ViewerConnection {
  public:
    ~WebEmbedConnection() override { nh_viewer_embed_uninstall(); }

    void poll() override {
        for (const auto &event : takeEvents()) {
            std::visit([this](const auto &message) { handle(message); }, event);
        }
        // App polls before idle gates, so the final coalesced pose is delivered
        // even when no further frames need rendering.
        if (ready_ && pending_ && Clock::now() - last_post_ >= std::chrono::milliseconds(100)) {
            post(*pending_);
        }
    }

  private:
    using Clock = std::chrono::steady_clock;
    bool ready_{false};
    Clock::time_point last_post_{};
    std::optional<events::CameraChanged> pending_;

    void handle(const events::ScenePresented &event) {
        if (!ready_) {
            ready_ = true;
            post(event);
        } else {
            // The embed contract announces initial readiness once. Later scene
            // reframing still updates the host's camera.
            handle(events::CameraChanged{event.camera});
        }
    }

    void handle(const events::CameraChanged &event) { pending_ = event; }

    void handle(const events::CameraResponse &event) {
        if (ready_) {
            post(event);
        }
    }

    void post(const events::Event &event) {
        nh_viewer_embed_post(protocol::eventToJson(event).dump().c_str());
        last_post_ = Clock::now();
        pending_.reset();
    }
};
} // namespace

std::shared_ptr<ViewerConnection> makeWebEmbedConnection() {
    auto connection = std::make_shared<WebEmbedConnection>();
    active_connection = connection;
    nh_viewer_embed_install();
    return connection;
}
} // namespace nodehammer::viewer

extern "C" {
EMSCRIPTEN_KEEPALIVE
void nh_viewer_embed_command(const char *command_json) {
    auto connection = nodehammer::viewer::active_connection.lock();
    if (!connection || command_json == nullptr) {
        return;
    }
    try {
        const auto command =
            nodehammer::viewer::protocol::commandFromJson(nlohmann::json::parse(command_json));
        if (command) {
            connection->submit(*command);
        }
    } catch (const std::exception &e) {
        std::println(stderr, "viewer: ignoring invalid command: {}", e.what());
    }
}
} // extern "C"

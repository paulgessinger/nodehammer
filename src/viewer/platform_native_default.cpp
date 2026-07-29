#include <nodehammer/viewer/platform.hpp>

#include "platform_native_common.hpp"

#include <nodehammer/viewer/app.hpp>

#include <sokol_app.h>

#include <span>
#include <utility>
#include <vector>

namespace nodehammer::viewer::platform {

struct Platform::Impl {
    App &app;
    NativePickerState pickers;
    WindowCustomizationRequest window_request;
    PlatformWindowState window_state;
    std::vector<PlatformGestureEvent> gesture_events;
};

Platform::Platform(App &app) : impl_(std::make_unique<Impl>(app)) {}
Platform::~Platform() = default;

void Platform::configureWindowDesc(sapp_desc & /*desc*/, const Config & /*cfg*/,
                                   const WindowCustomizationRequest &request) {
    impl_->window_request = request;
}

void Platform::attachWindow(const WindowCustomizationRequest &request) {
    impl_->window_request = request;
    impl_->window_state = {};
}

void Platform::handleWindowEvent(const sapp_event *ev) {
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        impl_->window_state.drag_hover.active = false;
    }
}

void Platform::beginFrameWindowSync() {}

const PlatformWindowState &Platform::windowState() const noexcept { return impl_->window_state; }

std::vector<PlatformGestureEvent> Platform::takeGestureEvents() {
    return std::exchange(impl_->gesture_events, {});
}

bool Platform::hasPendingGestures() const noexcept { return !impl_->gesture_events.empty(); }

void Platform::dispatchDroppedFiles() { dispatchNativeDroppedFiles(impl_->app); }

std::optional<std::string> Platform::loadPersistentText(const std::string &key) const {
    return loadNativePersistentText(key);
}

void Platform::savePersistentText(const std::string &key, const std::string &bytes) {
    saveNativePersistentText(key, bytes);
}

void Platform::commitUrlState(const std::string & /*state_query*/,
                              const std::string & /*managed_keys*/) {
    // No browser URL on native.
}

void Platform::openUrl(const std::string & /*url*/) {
    // TODO: native URL opener intentionally deferred.
}

std::optional<std::string> Platform::saveExportedImage(const std::string &filename,
                                                       std::span<const std::byte> bytes) {
    return saveNativeExportedImage(filename, bytes);
}

void Platform::downloadArchive(const std::string &, std::span<const std::byte>) {
    // Native archives are written to a picked path (saveArchivePicker); there is
    // no browser download.
}

// Web application-mode IDB persistence has no native equivalent (native modes
// persist through their own on-disk backing), so these are no-ops.
void Platform::loadProjectBlob() {}
void Platform::saveProjectBlob(std::span<const std::byte>) {}
void Platform::clearProjectBlob() {}

// Publish-package assembles the web runtime; native publish (copying a staged
// runtime) is not wired yet (§6.6), so this is a no-op.
void Platform::fetchRuntimeForPublish() {}

void Platform::openFilePicker() { impl_->pickers.openFilePicker(); }
void Platform::openFolderPicker() { impl_->pickers.openFolderPicker(); }
void Platform::openArchivePicker() { impl_->pickers.openArchivePicker(); }
void Platform::saveArchivePicker() { impl_->pickers.saveArchivePicker(); }
void Platform::drainPickers() { impl_->pickers.drainPickers(impl_->app); }

} // namespace nodehammer::viewer::platform

#include <nodehammer/viewer/platform.hpp>

#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/filesystem_project_fs.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#if defined(__APPLE__)
#include "platform_macos.hpp"
#endif

#include <nfd.hpp>
#include <sokol_app.h>

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace nodehammer::viewer::platform {

/// Native platform state. Picker latches live as members on this
/// instance — scoped to the owning App's lifetime, no file-static or
/// global state. NFD modals run inside `drainPickers` after the active
/// ImGui frame has been rendered, since NFD enters a nested run loop
/// on macOS / Windows that would re-enter sokol's frame_cb if invoked
/// inline.
struct Platform::Impl {
    App &app;
    WindowCustomizationRequest window_request;
    PlatformWindowState window_state;
    std::vector<PlatformGestureEvent> gesture_events;
    bool pending_file_picker{false};
    bool pending_folder_picker{false};

#if defined(__APPLE__)
    MacWindowCallbacks macCallbacks() {
        return MacWindowCallbacks{
            .user = this,
            .set_chrome =
                [](void *user, const WindowChromeState &chrome) {
                    static_cast<Impl *>(user)->window_state.chrome = chrome;
                },
            .set_drag_hover =
                [](void *user, const DragHoverState &drag_hover) {
                    static_cast<Impl *>(user)->window_state.drag_hover = drag_hover;
                },
            .push_gesture =
                [](void *user, const PlatformGestureEvent &event) {
                    static_cast<Impl *>(user)->gesture_events.push_back(event);
                },
        };
    }
#endif

    void runFilePickerModal() {
        NFD::Guard nfd;
        NFD::UniquePathSet picked;
        nfdu8filteritem_t filters[] = {
            {"Nodehammer scene", "toml,nhb,zst,gltf,glb,gdml,root,fb,json,xml"},
        };
        if (NFD::OpenDialogMultiple(picked, filters, 1) != NFD_OKAY) {
            return;
        }
        auto *project = app.project();
        if (project == nullptr) {
            return;
        }
        nfdpathsetsize_t count = 0;
        NFD::PathSet::Count(picked, count);
        for (nfdpathsetsize_t i = 0; i < count; ++i) {
            NFD::UniquePathSetPathU8 path;
            if (NFD::PathSet::GetPath(picked, i, path) == NFD_OKAY) {
                project->addPath(std::filesystem::path{path.get()});
            }
        }
    }

    void runFolderPickerModal() {
        NFD::Guard nfd;
        NFD::UniquePath picked;
        if (NFD::PickFolder(picked) != NFD_OKAY) {
            return;
        }
        app.setProject(std::make_unique<FilesystemProjectFs>(std::filesystem::path{picked.get()}));
    }
};

Platform::Platform(App &app) : impl_(std::make_unique<Impl>(app)) {}
Platform::~Platform() = default;

void Platform::configureWindowDesc(sapp_desc & /*desc*/, const Config & /*cfg*/,
                                   const WindowCustomizationRequest &request) {
    impl_->window_request = request;
}

void Platform::attachWindow(const WindowCustomizationRequest &request) {
    impl_->window_request = request;
    impl_->window_state.supports_window_restoration = false;
    impl_->window_state.supports_hidden_titlebar = false;
    impl_->window_state.supports_pinch_gesture = false;
#if defined(__APPLE__)
    impl_->window_state.supports_window_restoration = request.restore_placement;
    impl_->window_state.supports_hidden_titlebar = request.hide_titlebar_chrome;
    impl_->window_state.supports_pinch_gesture = request.track_platform_gestures;
    attachMacWindow(request, impl_->macCallbacks());
#endif
}

void Platform::handleWindowEvent(const sapp_event *ev) {
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        impl_->window_state.drag_hover.active = false;
    }
}

void Platform::beginFrameWindowSync() {
#if defined(__APPLE__)
    syncMacWindowState(impl_->macCallbacks());
#endif
}

const PlatformWindowState &Platform::windowState() const noexcept { return impl_->window_state; }

std::vector<PlatformGestureEvent> Platform::takeGestureEvents() {
    return std::exchange(impl_->gesture_events, {});
}

void Platform::dispatchDroppedFiles() {
    const int n = sapp_get_num_dropped_files();
    if (n == 0) {
        return;
    }
    auto *project = impl_->app.project();
    if (project == nullptr) {
        return;
    }
    // First-directory-wins: a folder drop replaces the project with a
    // fresh FilesystemProjectFs and ignores any sibling files in the
    // same gesture. Mixing folder + file drops is ambiguous (overlay /
    // union semantics) and a Stage 4+ concern.
    for (int i = 0; i < n; ++i) {
        std::filesystem::path p{sapp_get_dropped_file_path(i)};
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            impl_->app.setProject(std::make_unique<FilesystemProjectFs>(std::move(p)));
            return;
        }
    }
    for (int i = 0; i < n; ++i) {
        project->addPath(std::filesystem::path{sapp_get_dropped_file_path(i)});
    }
}

void Platform::commitUrlState(const std::string & /*state_query*/,
                              const std::string & /*managed_keys*/) {
    // No browser URL on native.
}

void Platform::openFilePicker() { impl_->pending_file_picker = true; }
void Platform::openFolderPicker() { impl_->pending_folder_picker = true; }

void Platform::drainPickers() {
    if (impl_->pending_file_picker) {
        impl_->pending_file_picker = false;
        impl_->runFilePickerModal();
    }
    if (impl_->pending_folder_picker) {
        impl_->pending_folder_picker = false;
        impl_->runFolderPickerModal();
    }
}

} // namespace nodehammer::viewer::platform

#include <nodehammer/viewer/platform.hpp>

#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/filesystem_project_fs.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <nfd.hpp>
#include <sokol_app.h>

#include <filesystem>
#include <memory>

namespace nodehammer::viewer::platform {

/// Native platform state. Picker latches live as members on this
/// instance — scoped to the owning App's lifetime, no file-static or
/// global state. NFD modals run inside `drainPickers` after the active
/// ImGui frame has been rendered, since NFD enters a nested run loop
/// on macOS / Windows that would re-enter sokol's frame_cb if invoked
/// inline.
struct Platform::Impl {
    App &app;
    bool pending_file_picker{false};
    bool pending_folder_picker{false};

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

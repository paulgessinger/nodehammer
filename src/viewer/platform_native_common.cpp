#include "platform_native_common.hpp"

#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/filesystem_project_fs.hpp>
#include <nodehammer/viewer/native_bag_project_fs.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <nfd.hpp>
#include <sokol_app.h>

#include <filesystem>
#include <memory>

namespace nodehammer::viewer::platform {

std::unique_ptr<ProjectFs> makeEmptyBag() { return std::make_unique<NativeBagProjectFs>(); }

void NativePickerState::openFilePicker() { pending_file_picker_ = true; }
void NativePickerState::openFolderPicker() { pending_folder_picker_ = true; }

void NativePickerState::drainPickers(App &app) {
    if (pending_file_picker_) {
        pending_file_picker_ = false;
        runFilePickerModal(app);
    }
    if (pending_folder_picker_) {
        pending_folder_picker_ = false;
        runFolderPickerModal(app);
    }
}

void NativePickerState::runFilePickerModal(App &app) {
    NFD::Guard nfd;
    NFD::UniquePathSet picked;
    nfdu8filteritem_t filters[] = {
        {"Nodehammer scene", "toml,nhb,zst,gltf,glb,gdml,root,fb,json,xml"},
    };
    if (NFD::OpenDialogMultiple(picked, filters, 1) != NFD_OKAY) {
        return;
    }
    if (app.project() == nullptr) {
        return;
    }
    nfdpathsetsize_t count = 0;
    NFD::PathSet::Count(picked, count);
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        NFD::UniquePathSetPathU8 path;
        if (NFD::PathSet::GetPath(picked, i, path) == NFD_OKAY) {
            app.addProjectPath(std::filesystem::path{path.get()});
        }
    }
}

void NativePickerState::runFolderPickerModal(App &app) {
    NFD::Guard nfd;
    NFD::UniquePath picked;
    if (NFD::PickFolder(picked) != NFD_OKAY) {
        return;
    }
    app.setProject(std::make_unique<FilesystemProjectFs>(std::filesystem::path{picked.get()}));
}

void dispatchNativeDroppedFiles(App &app) {
    const int n = sapp_get_num_dropped_files();
    if (n == 0) {
        return;
    }
    if (app.project() == nullptr) {
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
            app.setProject(std::make_unique<FilesystemProjectFs>(std::move(p)));
            return;
        }
    }
    for (int i = 0; i < n; ++i) {
        app.addProjectPath(std::filesystem::path{sapp_get_dropped_file_path(i)});
    }
}

} // namespace nodehammer::viewer::platform

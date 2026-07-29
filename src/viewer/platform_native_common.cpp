#include "platform_native_common.hpp"

#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/archive_project_fs.hpp>
#include <nodehammer/viewer/filesystem_project_fs.hpp>
#include <nodehammer/viewer/native_bag_project_fs.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/project_fs.hpp>
#include <nodehammer/viewer/watched_filesystem_project_fs.hpp>

#ifdef NH_VIEWER_NATIVE_DIALOG
#include <nfd.hpp>
#endif
#include <sago/platform_folders.h>
#include <sokol_app.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>

namespace nodehammer::viewer::platform {
namespace {

std::filesystem::path persistentTextPath(const std::string &key) {
    return std::filesystem::path{sago::getConfigHome()} / "nodehammer" / key;
}

/// Case-insensitive ".nhproj" extension check for archive-drop detection.
bool isProjectArchivePath(const std::filesystem::path &p) {
    auto ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".nhproj";
}

} // namespace

std::unique_ptr<ProjectFs> makeEmptyBag() { return std::make_unique<NativeBagProjectFs>(); }

void NativePickerState::openFilePicker() { pending_file_picker_ = true; }
void NativePickerState::openFolderPicker() { pending_folder_picker_ = true; }
void NativePickerState::openArchivePicker() { pending_archive_picker_ = true; }
void NativePickerState::saveArchivePicker() { pending_save_archive_picker_ = true; }

void NativePickerState::drainPickers(App &app) {
    if (pending_file_picker_) {
        pending_file_picker_ = false;
        runFilePickerModal(app);
    }
    if (pending_folder_picker_) {
        pending_folder_picker_ = false;
        runFolderPickerModal(app);
    }
    if (pending_archive_picker_) {
        pending_archive_picker_ = false;
        runArchivePickerModal(app);
    }
    if (pending_save_archive_picker_) {
        pending_save_archive_picker_ = false;
        runSaveArchivePickerModal(app);
    }
}

#ifdef NH_VIEWER_NATIVE_DIALOG

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
    app.setProject(std::make_unique<WatchedFilesystemProjectFs>(
        std::make_unique<FilesystemProjectFs>(std::filesystem::path{picked.get()})));
}

void NativePickerState::runArchivePickerModal(App &app) {
    NFD::Guard nfd;
    NFD::UniquePath picked;
    nfdu8filteritem_t filters[] = {{"Nodehammer project", "nhproj"}};
    if (NFD::OpenDialog(picked, filters, 1) != NFD_OKAY) {
        return;
    }
    app.setProject(std::make_unique<ArchiveProjectFs>(std::filesystem::path{picked.get()}));
}

void NativePickerState::runSaveArchivePickerModal(App &app) {
    NFD::Guard nfd;
    NFD::UniquePath picked;
    nfdu8filteritem_t filters[] = {{"Nodehammer project", "nhproj"}};
    if (NFD::SaveDialog(picked, filters, 1, nullptr, "project.nhproj") != NFD_OKAY) {
        return;
    }
    std::filesystem::path path{picked.get()};
    if (!isProjectArchivePath(path)) {
        path += ".nhproj";
    }
    app.saveActiveArchiveTo(path);
}

#else // NH_VIEWER_NATIVE_DIALOG

// Built without NFD (NODEHAMMER_VIEWER_NATIVE_DIALOG=OFF): no GTK/DBus
// dependency, so the picker is a no-op. Drag-and-drop and the CLI --input
// flag remain the ways to load geometry.
void NativePickerState::runFilePickerModal(App &) {
    std::println(stderr, "viewer: built without a native file dialog; "
                         "drag files onto the window or pass --input on the CLI.");
}

void NativePickerState::runFolderPickerModal(App &) {
    std::println(stderr, "viewer: built without a native folder dialog; "
                         "drag a folder onto the window instead.");
}

void NativePickerState::runArchivePickerModal(App &) {
    std::println(stderr, "viewer: built without a native file dialog; "
                         "drag a .nhproj archive onto the window instead.");
}

void NativePickerState::runSaveArchivePickerModal(App &) {
    std::println(stderr, "viewer: built without a native file dialog; "
                         "cannot pick a save path for the archive.");
}

#endif // NH_VIEWER_NATIVE_DIALOG

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
            app.setProject(std::make_unique<WatchedFilesystemProjectFs>(
                std::make_unique<FilesystemProjectFs>(std::move(p))));
            return;
        }
    }
    // A single dropped .nhproj replaces the project with a live archive mode. Only
    // the sole-file case is unambiguous; a .nhproj mixed with other files falls
    // through to flat ingestion below.
    if (n == 1) {
        std::filesystem::path p{sapp_get_dropped_file_path(0)};
        if (isProjectArchivePath(p)) {
            app.setProject(std::make_unique<ArchiveProjectFs>(std::move(p)));
            return;
        }
    }
    for (int i = 0; i < n; ++i) {
        app.addProjectPath(std::filesystem::path{sapp_get_dropped_file_path(i)});
    }
}

std::optional<std::string> loadNativePersistentText(const std::string &key) {
    const auto path = persistentTextPath(key);
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        return std::nullopt;
    }
    const auto size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (!bytes.empty() && !in.read(bytes.data(), size)) {
        return std::nullopt;
    }
    return bytes;
}

void saveNativePersistentText(const std::string &key, const std::string &bytes) {
    const auto path = persistentTextPath(key);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        std::println(stderr, "viewer: persistent state mkdir failed ({}): {}",
                     path.parent_path().string(), ec.message());
        return;
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        std::println(stderr, "viewer: persistent state open-for-write failed: {}", path.string());
        return;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::optional<std::string> saveNativeExportedImage(const std::string &filename,
                                                   std::span<const std::byte> bytes) {
    std::error_code ec;
    const auto path = std::filesystem::current_path(ec) / filename;
    if (ec) {
        std::println(stderr, "viewer: export image cwd lookup failed: {}", ec.message());
        return std::nullopt;
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        std::println(stderr, "viewer: export image open-for-write failed: {}", path.string());
        return std::nullopt;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::println(stderr, "viewer: export image write failed: {}", path.string());
        return std::nullopt;
    }
    return path.string();
}

} // namespace nodehammer::viewer::platform

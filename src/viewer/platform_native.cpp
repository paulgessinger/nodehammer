#include <nodehammer/viewer/platform.hpp>

#include <nodehammer/viewer/asset_source.hpp>

#include <nfd.hpp>
#include <sokol_app.h>

#include <filesystem>

namespace nodehammer::viewer::platform {

void commitUrlState(const std::string & /*state_query*/, const std::string & /*managed_keys*/) {
    // No browser URL on native.
}

void dispatchWebFilePicker() {
    // Native uses runNativeFilePicker; the web inline-dispatch path doesn't
    // apply.
}

void dispatchDroppedFiles(AssetSource &source) {
    const int n = sapp_get_num_dropped_files();
    for (int i = 0; i < n; ++i) {
        source.ingestLocalFile(std::filesystem::path{sapp_get_dropped_file_path(i)});
    }
}

void runNativeFilePicker(const NativeFilePathHandler &handler) {
    if (!handler) {
        return;
    }
    NFD::Guard nfd;
    NFD::UniquePathSet picked;
    nfdu8filteritem_t filters[] = {
        {"Nodehammer scene", "toml,nhb,zst,gltf,glb,gdml,root,fb,json,xml"},
    };
    if (NFD::OpenDialogMultiple(picked, filters, 1) != NFD_OKAY) {
        return;
    }
    nfdpathsetsize_t count = 0;
    NFD::PathSet::Count(picked, count);
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        NFD::UniquePathSetPathU8 path;
        if (NFD::PathSet::GetPath(picked, i, path) == NFD_OKAY) {
            handler(std::filesystem::path{path.get()});
        }
    }
}

} // namespace nodehammer::viewer::platform

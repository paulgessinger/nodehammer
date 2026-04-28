#include <nodehammer/viewer/platform.hpp>

#include <nfd.hpp>

#include <filesystem>

namespace nodehammer::viewer::platform {

void commitUrlState(const std::string & /*state_query*/, const std::string & /*managed_keys*/) {
    // No browser URL on native.
}

void dispatchWebFilePicker() {
    // Native uses runNativeFilePicker; the web inline-dispatch path doesn't
    // apply.
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

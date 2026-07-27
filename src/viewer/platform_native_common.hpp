#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace nodehammer::viewer {
class App;
}

namespace nodehammer::viewer::platform {

class NativePickerState {
  public:
    void openFilePicker();
    void openFolderPicker();
    void openArchivePicker();
    void saveArchivePicker();
    void drainPickers(App &app);

  private:
    bool pending_file_picker_{false};
    bool pending_folder_picker_{false};
    bool pending_archive_picker_{false};
    bool pending_save_archive_picker_{false};

    static void runFilePickerModal(App &app);
    static void runFolderPickerModal(App &app);
    static void runArchivePickerModal(App &app);
    static void runSaveArchivePickerModal(App &app);
};

void dispatchNativeDroppedFiles(App &app);
[[nodiscard]] std::optional<std::string> loadNativePersistentText(const std::string &key);
void saveNativePersistentText(const std::string &key, const std::string &bytes);

/// Write an exported image to the current working directory. Returns the
/// absolute path on success, or nullopt on failure.
[[nodiscard]] std::optional<std::string> saveNativeExportedImage(const std::string &filename,
                                                                 std::span<const std::byte> bytes);

} // namespace nodehammer::viewer::platform

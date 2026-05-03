#pragma once

#include <optional>
#include <string>

namespace nodehammer::viewer {
class App;
}

namespace nodehammer::viewer::platform {

class NativePickerState {
  public:
    void openFilePicker();
    void openFolderPicker();
    void drainPickers(App &app);

  private:
    bool pending_file_picker_{false};
    bool pending_folder_picker_{false};

    static void runFilePickerModal(App &app);
    static void runFolderPickerModal(App &app);
};

void dispatchNativeDroppedFiles(App &app);
[[nodiscard]] std::optional<std::string> loadNativePersistentText(const std::string &key);
void saveNativePersistentText(const std::string &key, const std::string &bytes);

} // namespace nodehammer::viewer::platform

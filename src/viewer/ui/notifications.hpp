#pragma once

#include <string_view>

namespace nodehammer::viewer::ui::Notifications {

void initializeFonts();

void info(std::string_view message);
void success(std::string_view message);
void warning(std::string_view message);
void error(std::string_view message);

void render();

} // namespace nodehammer::viewer::ui::Notifications

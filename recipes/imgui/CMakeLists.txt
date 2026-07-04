cmake_minimum_required(VERSION 3.15)
project(imgui LANGUAGES CXX)

add_library(imgui
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
)

target_include_directories(imgui PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends>"
    "$<INSTALL_INTERFACE:include>"
    "$<INSTALL_INTERFACE:include/backends>"
)

install(TARGETS imgui ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES
    imconfig.h
    imgui.h
    imgui_internal.h
    imstb_rectpack.h
    imstb_textedit.h
    imstb_truetype.h
    DESTINATION include
)
install(DIRECTORY backends/ DESTINATION include/backends FILES_MATCHING PATTERN "*.h")

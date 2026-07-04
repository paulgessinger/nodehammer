cmake_minimum_required(VERSION 3.15)
project(implot LANGUAGES CXX)

find_package(imgui CONFIG REQUIRED)

add_library(implot
    implot.cpp
    implot_items.cpp
    implot_demo.cpp
)

target_include_directories(implot PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(implot PUBLIC ImGui::ImGui)

install(TARGETS implot ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES
    implot.h
    implot_internal.h
    DESTINATION include
)

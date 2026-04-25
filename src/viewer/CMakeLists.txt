add_library(nodehammer_viewer STATIC
    app.cpp
    sdl_platform.cpp
    imgui_impl_bgfx.cpp
)

target_include_directories(nodehammer_viewer PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_include_directories(nodehammer_viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Embedded-shader headers are written under shaders/${CMAKE_BUILD_DIR}/shaders/
# at configure-time via bgfx_compile_shader_to_header (see shaders/CMakeLists.txt).
target_include_directories(nodehammer_viewer PRIVATE
    ${CMAKE_BINARY_DIR}/shaders
)

target_link_libraries(nodehammer_viewer PUBLIC
    bgfx
    bx
    SDL3::SDL3-static
    imgui
    glm::glm
)

# The shader headers are produced by the shaders/ subdirectory; a target-level
# dependency keeps the build order correct without baking specific files in.
add_dependencies(nodehammer_viewer nodehammer_shaders)

nodehammer_set_compiler_options(nodehammer_viewer)

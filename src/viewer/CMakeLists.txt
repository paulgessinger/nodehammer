# The viewer is composed of plain C++23 sources that compile against
# whichever sokol::sokol_<backend> they're linked into. Because sokol_gfx.h
# / sokol_app.h headers expose backend-dependent struct surfaces, the same
# .cpp files have to be compiled separately for each backend executable —
# so we don't build a STATIC library here. Instead, expose the source list
# (and the include path for the sokol-shdc-generated shader header) up to
# the parent so the top-level CMakeLists.txt can drop the sources straight
# onto each viewer executable target.

set(NH_VIEWER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/app.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ibl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/url_asset_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/local_file_asset_source.cpp
    PARENT_SCOPE
)

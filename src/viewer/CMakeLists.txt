# The viewer is composed of plain C++23 sources that compile against
# whichever sokol::sokol_<backend> they're linked into. Because sokol_gfx.h
# / sokol_app.h headers expose backend-dependent struct surfaces, the same
# .cpp files have to be compiled separately for each backend executable —
# so we don't build a STATIC library here. Instead, expose the source list
# (and the include path for the sokol-shdc-generated shader header) up to
# the parent so the top-level CMakeLists.txt can drop the sources straight
# onto each viewer executable target.
#
# Per-platform splits (selected via $<IF:$<BOOL:${EMSCRIPTEN}>,…,…>):
#   ibl_*               — bake job runs on a worker thread (native) or
#                         frame-time-sliced iterator (web)
#   ibl_cache_*         — file under sago::getCacheDir (native) or
#                         IndexedDB (web)
#   scene_build_job_*   — std::thread (native) or polled state machine (web)
#   url_asset_source_*  — stub (native) or emscripten_fetch driver (web)
#   platform_*          — EM_JS / KEEPALIVE shims live in the web variant

set(NH_VIEWER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/app.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ibl_common.cpp
    $<IF:$<BOOL:${EMSCRIPTEN}>,${CMAKE_CURRENT_SOURCE_DIR}/ibl_web.cpp,${CMAKE_CURRENT_SOURCE_DIR}/ibl_native.cpp>
    ${CMAKE_CURRENT_SOURCE_DIR}/ibl_cache_common.cpp
    $<IF:$<BOOL:${EMSCRIPTEN}>,${CMAKE_CURRENT_SOURCE_DIR}/ibl_cache_web.cpp,${CMAKE_CURRENT_SOURCE_DIR}/ibl_cache_native.cpp>
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_build_job_common.cpp
    $<IF:$<BOOL:${EMSCRIPTEN}>,${CMAKE_CURRENT_SOURCE_DIR}/scene_build_job_web.cpp,${CMAKE_CURRENT_SOURCE_DIR}/scene_build_job_native.cpp>
    $<IF:$<BOOL:${EMSCRIPTEN}>,${CMAKE_CURRENT_SOURCE_DIR}/url_asset_source_web.cpp,${CMAKE_CURRENT_SOURCE_DIR}/url_asset_source_native.cpp>
    $<IF:$<BOOL:${EMSCRIPTEN}>,${CMAKE_CURRENT_SOURCE_DIR}/platform_web.cpp,${CMAKE_CURRENT_SOURCE_DIR}/platform_native.cpp>
    ${CMAKE_CURRENT_SOURCE_DIR}/local_file_asset_source.cpp
    PARENT_SCOPE
)

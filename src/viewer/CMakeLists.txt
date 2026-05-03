# sokol_gfx.h / sokol_app.h expose backend-dependent struct surfaces, so
# every viewer TU has to be compiled against exactly one SOKOL_<backend>
# define. Native targets one backend per host and is straightforward; the
# wasm build produces TWO viewer executables (gles3 + wgpu) from a single
# configure, so the viewer code has to be compiled twice with different
# defines.
#
# Solution: parameterised STATIC lib. `nh_add_viewer_lib(name sokol_lib)`
# creates one static lib per backend, links the matching sokol_<backend>
# PUBLICly so the backend define + sokol headers reach every viewer TU,
# and pulls in the platform-specific dependencies (NFD) on native.
# The root CMakeLists then composes one (native) or two (wasm)
# such libs and links them into thin executable targets.
#
# Per-platform splits (selected via $<IF:$<BOOL:${EMSCRIPTEN}>,…,…>):
#   scene_build_job_*   — std::thread (native) or polled state machine (web)
#   platform_*          — EM_JS / KEEPALIVE shims live in the web variant
#
# url_project_fs.cpp is web-only: native already builds synchronously
# from --input/--config in cmd_viewer.cpp, so the network fetch path has
# no meaning off the browser.

# Compose one viewer static lib for the given sokol_<backend> lib. PUBLIC
# linkage on sokol_lib + nodehammer_lib so consumer executables inherit
# their includes/defines transitively. Source paths are anchored on
# CMAKE_SOURCE_DIR (a global) because CMake function bodies resolve
# variables in the caller's scope at call time — and this helper is
# invoked from the root CMakeLists, where CMAKE_CURRENT_SOURCE_DIR no
# longer points here.
function(nh_add_viewer_lib name sokol_lib)
    set(_dir "${CMAKE_SOURCE_DIR}/src/viewer")
    add_library(${name} STATIC
        ${_dir}/app.cpp
        ${_dir}/camera.cpp
        ${_dir}/scene_renderer.cpp
        ${_dir}/imgui_backend.cpp
        ${_dir}/ui/debug_panel.cpp
        ${_dir}/ui/icon_font.cpp
        ${_dir}/ui/menu_bar.cpp
        ${_dir}/ui/notifications.cpp
        ${_dir}/ui/project_panel.cpp
        ${_dir}/ui/status_ui.cpp
        ${_dir}/ui/view_panel.cpp
        ${_dir}/ui/viewer_ui.cpp
        ${_dir}/ibl.cpp
        ${_dir}/scene_build_job_common.cpp
        $<IF:$<BOOL:${EMSCRIPTEN}>,${_dir}/scene_build_job_web.cpp,${_dir}/scene_build_job_native.cpp>
        $<IF:$<BOOL:${EMSCRIPTEN}>,${_dir}/url_project_fs_web.cpp,>
        $<IF:$<BOOL:${EMSCRIPTEN}>,${_dir}/platform_web.cpp,$<IF:$<BOOL:${APPLE}>,${_dir}/platform_macos.mm,${_dir}/platform_native_default.cpp>>
        $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:${_dir}/platform_native_common.cpp>
        # bag_project_fs.cpp + build_session.cpp now live in nodehammer_lib
        # (no sokol deps, headless `buildSceneFromPaths` shares them).
    )
    target_link_libraries(${name} PUBLIC nodehammer_lib ${sokol_lib} nh_fontawesome7)
    target_include_directories(${name} PRIVATE ${NH_SHADER_INCLUDE_DIR})
    add_dependencies(${name} nodehammer_shaders)
    nodehammer_set_compiler_options(${name})

    if(NOT EMSCRIPTEN)
        # Native file picker (NFD-extended). The web build uses an HTML
        # <input type=file> + FileSystemAccess API, so it never needs this
        # dep. nfd::nfd is only declared when NOT EMSCRIPTEN — see
        # cmake/Dependencies.cmake.
        target_link_libraries(${name} PUBLIC nfd::nfd)
    endif()
endfunction()

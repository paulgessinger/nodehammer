# Sokol viewer dependencies — sokol headers via Conan, sokol-shdc
# resolved as an IMPORTED executable from the build PATH (typically supplied
# by the local Conan recipe under recipes/sokol-shdc).
#
# Provides:
#   - INTERFACE target sokol::headers      — bare header includes
#   - IMPORTED executable sokol::shdc      — shader compiler
#   - function nh_add_sokol_lib(...)       — STATIC lib of impl TUs per backend
#   - function nh_compile_shader(...)      — invokes sokol-shdc on a .glsl file
#
# Single CMake configure produces multiple sokol_* libs and exes (one per
# backend on Emscripten — GLES3 + WGPU); see top-level CMakeLists.txt for the
# call sites.

include(CMakeParseArguments)
find_package(sokol REQUIRED CONFIG)

# ── sokol headers ────────────────────────────────────────────────────────────
# Pinned by recipes/sokol, including nodehammer's WGPU null-vertex-buffer
# workaround for emdawnwebgpu.
# ── sokol-shdc binary (resolved from PATH / Conan tool_requires) ─────────────
# Mirrors the bgfx::shaderc resolver pattern that lived in cmake/Dependencies.cmake
# before this rewrite. Conan's CMakeToolchain prepends build-context tool dirs
# to CMAKE_PROGRAM_PATH so find_program sees the binary supplied by the
# sokol-shdc Conan tool_requires.
if(NOT TARGET sokol::shdc)
    find_program(NODEHAMMER_HOST_SOKOL_SHDC sokol-shdc
        DOC "sokol-shdc binary, typically supplied via Conan tool_requires")
    if(NODEHAMMER_HOST_SOKOL_SHDC)
        add_executable(sokol::shdc IMPORTED GLOBAL)
        set_property(TARGET sokol::shdc PROPERTY
            IMPORTED_LOCATION "${NODEHAMMER_HOST_SOKOL_SHDC}")
    else()
        message(FATAL_ERROR
            "NODEHAMMER_WITH_VIEWER is ON but sokol-shdc was not found on PATH. "
            "Run `just recipes && just deps` (or `just wasm-deps`) — the local "
            "Conan recipe at recipes/sokol-shdc/ ships the prebuilt binary.")
    endif()
endif()

# ── nh_silence_sokol_warnings(target) ────────────────────────────────────────
# The sokol implementation TUs are platform-dense C/Obj-C/C++ code that flags
# every -Wpedantic style we set on first-party code. Suppress on the per-target
# basis; do not propagate.
function(nh_silence_sokol_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -w)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W0)
    endif()
endfunction()

# ── nh_add_sokol_lib(name backend_define [link_libs...]) ─────────────────────
# Builds a STATIC library that compiles the sokol implementation TUs with one
# specific backend define. The backend define + the platform link libraries
# are PUBLIC so executables linking against this lib pick them up transitively.
#
# Native: called once per host (e.g. SOKOL_METAL on macOS).
# Emscripten: called twice (SOKOL_GLES3 + SOKOL_WGPU) so a single CMake configure
# produces both wasm bundles for the runtime-detect viewer shell.
set(NH_SOKOL_IMPL_DIR ${CMAKE_SOURCE_DIR}/src/sokol)

if(APPLE)
    # Apple: sokol_app + sokol_gfx (Metal) require Objective-C(++) because
    # they reach into AppKit / Metal. Sokol's single-header impl model means
    # SOKOL_IMPL must appear in exactly one TU per program — split into two:
    # sokol_impl.mm bundles the C-only sokol headers, sokol_imgui_impl.mm
    # implements the imgui backend (which needs C++).
    set(NH_SOKOL_IMPL_SOURCES
        ${NH_SOKOL_IMPL_DIR}/sokol_impl.mm
        ${NH_SOKOL_IMPL_DIR}/sokol_imgui_impl.mm
    )
else()
    set(NH_SOKOL_IMPL_SOURCES
        ${NH_SOKOL_IMPL_DIR}/sokol_impl.c
        ${NH_SOKOL_IMPL_DIR}/sokol_imgui_impl.cc
    )
endif()

function(nh_add_sokol_lib name backend_define)
    add_library(${name} STATIC ${NH_SOKOL_IMPL_SOURCES})
    target_link_libraries(${name} PUBLIC sokol::headers ImGui::ImGui)
    # SOKOL_NO_ENTRY: we provide our own main() (the CLI dispatcher in
    # src/cli/main.cpp) and call sapp_run() from inside the viewer
    # subcommand. Without this, sokol_app.h synthesises a main() which
    # would conflict with the CLI's entry point.
    target_compile_definitions(${name} PUBLIC ${backend_define} SOKOL_NO_ENTRY)
    # Per-platform link surface. PUBLIC so executables get them transitively.
    if(APPLE)
        target_link_libraries(${name} PUBLIC
            "-framework Cocoa"
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore"
            "-framework Foundation"
            "-framework AudioToolbox"
        )
    elseif(UNIX AND NOT EMSCRIPTEN)
        find_package(Threads REQUIRED)
        target_link_libraries(${name} PUBLIC X11 Xi Xcursor GL dl Threads::Threads)
    elseif(WIN32)
        target_link_libraries(${name} PUBLIC user32 gdi32 ole32 shell32 winmm)
    endif()
    # Caller may pass additional libs (e.g. for the WGPU backend if it ever
    # needs Dawn on native; today it's a no-op on every supported host).
    if(ARGN)
        target_link_libraries(${name} PUBLIC ${ARGN})
    endif()
    nh_silence_sokol_warnings(${name})
endfunction()

# ── nh_compile_shader(<input.glsl> OUT_HEADER <var> SLANGS <slang-list> [DEPENDS_TARGET <tgt>] [INCLUDES <files...>]) ──
# Wraps a single sokol-shdc invocation with add_custom_command. Sets ${var} to
# the absolute path of the generated header, suitable for #include from a
# viewer source file.
#
# SLANGS is a colon-separated list (sokol-shdc's native syntax) — e.g.
# "glsl300es:wgsl" produces both backends from one .glsl input, with
# `--ifdef` wrapping each backend's blob in #if defined(SOKOL_*).
#
# INCLUDES lists shared snippet files (typically `*.glsl.h`) the input pulls
# in via sokol-shdc's `@include` directive. They become build dependencies of
# the custom command so edits to a helper trigger a rerun of every consumer.
# Without this an edit silently leaves stale generated headers.
function(nh_compile_shader input)
    cmake_parse_arguments(NH "" "OUT_HEADER;DEPENDS_TARGET" "SLANGS;INCLUDES" ${ARGN})
    if(NOT NH_OUT_HEADER OR NOT NH_SLANGS)
        message(FATAL_ERROR "nh_compile_shader: OUT_HEADER and SLANGS are required")
    endif()

    get_filename_component(_stem "${input}" NAME_WE)
    set(_in_abs  "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    set(_out_abs "${_out_dir}/${_stem}.glsl.h")

    # Resolve INCLUDES (caller may pass relative paths) to absolute paths so
    # add_custom_command's DEPENDS sees the same path regardless of caller cwd.
    set(_include_deps "")
    foreach(_inc IN LISTS NH_INCLUDES)
        if(IS_ABSOLUTE "${_inc}")
            list(APPEND _include_deps "${_inc}")
        else()
            list(APPEND _include_deps "${CMAKE_CURRENT_SOURCE_DIR}/${_inc}")
        endif()
    endforeach()

    # Join the slang list with ':' for sokol-shdc.
    string(REPLACE ";" ":" _slang_arg "${NH_SLANGS}")

    add_custom_command(
        OUTPUT  "${_out_abs}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
        COMMAND $<TARGET_FILE:sokol::shdc>
                -i "${_in_abs}"
                -o "${_out_abs}"
                -l "${_slang_arg}"
                -f sokol
                --ifdef
                --reflection
        DEPENDS "${_in_abs}" ${_include_deps} sokol::shdc
        COMMENT "sokol-shdc ${_stem}.glsl -> ${_stem}.glsl.h (${_slang_arg})"
        VERBATIM
    )

    set(${NH_OUT_HEADER} "${_out_abs}" PARENT_SCOPE)

    if(NH_DEPENDS_TARGET)
        # Caller wants a target-level dependency so multiple consumers don't
        # race on the same custom command output. Each shader gets its own
        # private custom target wrapping the generated header; the umbrella
        # NH_DEPENDS_TARGET (created on the first call) depends on all of
        # them, so consumers only need to depend on the umbrella.
        set(_per_shader_target "nh_shader_${_stem}")
        add_custom_target(${_per_shader_target} DEPENDS "${_out_abs}")
        if(NOT TARGET ${NH_DEPENDS_TARGET})
            add_custom_target(${NH_DEPENDS_TARGET})
        endif()
        add_dependencies(${NH_DEPENDS_TARGET} ${_per_shader_target})
    endif()
endfunction()

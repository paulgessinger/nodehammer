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

# This module's own directory, captured at include time. CMAKE_CURRENT_LIST_DIR
# inside a function resolves to the *caller's* list dir, so functions below
# reference sibling scripts (e.g. validate_shader_hlsl.cmake) through this.
set(NH_SOKOL_LIST_DIR "${CMAKE_CURRENT_LIST_DIR}")

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

# ── Cross-compiled shader validation ─────────────────────────────────────────
# The GPU backends cross-compile these GLSL sources to their native shading
# language and only compile that at runtime, so back-end compiler diagnostics
# never fail the build — they surface as viewer log spam. When ON (default),
# nh_compile_shader validates each backend's output at build time:
#   - HLSL (hlsl5, Windows)     → fxc /WX   (fxc ships with the Windows SDK)
#   - WGSL (wgsl, Emscripten)   → naga      (`cargo install naga-cli`)
# Each is a no-op on platforms that don't emit that backend. The
# NODEHAMMER_SHADER_STRICT option itself is declared in the top-level
# CMakeLists.txt (all options live there); it is ON by default.

# GitHub Actions (and most CI) set CI=true. In CI a missing validator is a hard
# error — the check must never silently no-op there — while locally it degrades
# to a warning so a dev without the tool isn't blocked.
set(_nh_ci FALSE)
if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
    set(_nh_ci TRUE)
endif()

# nh_require_validator(<pretty-name> <found-var> <hint>): FATAL in CI, WARNING
# locally when a validator binary is missing.
function(nh_require_validator name found hint)
    if(${found})
        return()
    endif()
    if(_nh_ci)
        message(FATAL_ERROR
            "NODEHAMMER_SHADER_STRICT is ON and this is a CI run, but ${name} was "
            "not found — shader validation would be silently skipped in CI. ${hint}")
    else()
        message(WARNING
            "NODEHAMMER_SHADER_STRICT is ON but ${name} was not found; the "
            "matching shader validation will be skipped. ${hint}")
    endif()
endfunction()

if(NODEHAMMER_SHADER_STRICT AND WIN32)
    find_program(NODEHAMMER_FXC fxc DOC "D3DCompile CLI (fxc.exe) from the Windows SDK")
    nh_require_validator("fxc.exe (Windows SDK)" NODEHAMMER_FXC
        "Build from an MSVC/SDK environment, or set NODEHAMMER_FXC.")
endif()

if(NODEHAMMER_SHADER_STRICT AND EMSCRIPTEN)
    find_program(NODEHAMMER_NAGA naga
        HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
        DOC "naga-cli WGSL validator (cargo install naga-cli)")
    nh_require_validator("naga (naga-cli)" NODEHAMMER_NAGA
        "Run `cargo install naga-cli`.")
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

    # Optional fxc /WX validation of the hlsl5 output, appended as an extra
    # build step so it re-runs whenever the shader changes (and fails the build
    # on any D3DCompile warning). Only meaningful when the shader actually
    # targets hlsl5 and fxc was located.
    set(_validate_cmd "")
    if(NODEHAMMER_SHADER_STRICT AND WIN32 AND NODEHAMMER_FXC AND "hlsl5" IN_LIST NH_SLANGS)
        set(_validate_cmd
            COMMAND ${CMAKE_COMMAND}
                    -DSHDC=$<TARGET_FILE:sokol::shdc>
                    -DFXC=${NODEHAMMER_FXC}
                    -DINPUT=${_in_abs}
                    -DSTEM=${_stem}
                    -DTMPDIR=${_out_dir}/hlsl_validate/${_stem}
                    -P ${NH_SOKOL_LIST_DIR}/validate_shader_hlsl.cmake)
    endif()

    # Same idea for the wgsl output (Emscripten builds): validate each stage
    # with naga so a bad WGSL cross-compile fails the build instead of only
    # erroring in the browser at runtime.
    set(_validate_wgsl_cmd "")
    if(NODEHAMMER_SHADER_STRICT AND EMSCRIPTEN AND NODEHAMMER_NAGA AND "wgsl" IN_LIST NH_SLANGS)
        set(_validate_wgsl_cmd
            COMMAND ${CMAKE_COMMAND}
                    -DSHDC=$<TARGET_FILE:sokol::shdc>
                    -DNAGA=${NODEHAMMER_NAGA}
                    -DINPUT=${_in_abs}
                    -DSTEM=${_stem}
                    -DTMPDIR=${_out_dir}/wgsl_validate/${_stem}
                    -P ${NH_SOKOL_LIST_DIR}/validate_shader_wgsl.cmake)
    endif()

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
        ${_validate_cmd}
        ${_validate_wgsl_cmd}
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

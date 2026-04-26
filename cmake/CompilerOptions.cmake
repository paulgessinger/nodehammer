# Sets C++23 compiler warnings and optional sanitizers on a target.
# Call nodehammer_set_compiler_options(target) for each target.

function(nodehammer_set_compiler_options target)
    # Warnings are PRIVATE: do not propagate to dependents (and not to FetchContent subprojects).
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wnon-virtual-dtor
            -Woverloaded-virtual
        )
        # Clang 19+ flags Catch2's use of __COUNTER__ under -Wpedantic as a
        # "C2y extension". __COUNTER__ has been a de facto standard extension
        # across gcc/clang/msvc for ages; silence the new warning class.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
                AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19)
            target_compile_options(${target} PRIVATE -Wno-c2y-extensions)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /external:anglebrackets
            /external:W0
            /external:templates-
        )
    endif()

    if(NODEHAMMER_WERROR)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE -Werror)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /WX)
        endif()
    endif()

    if(NODEHAMMER_ENABLE_ASAN)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            # ASan must propagate to consumers linking this target (static lib + exe).
            target_compile_options(${target} PUBLIC -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PUBLIC -fsanitize=address)
        else()
            message(WARNING "ASAN requested but compiler is not Clang/GCC; ignoring.")
        endif()
    endif()
endfunction()

# Apply link options that Emscripten executables need to run under node:
# NODERAWFS exposes the host filesystem (so fixture paths resolve), EXIT_RUNTIME
# makes the process return the C++ exit code, and ALLOW_MEMORY_GROWTH avoids
# OOM aborts on larger test inputs. No-op on non-emscripten builds.
function(nodehammer_apply_emscripten_exe_options target)
    if(EMSCRIPTEN)
        target_link_options(${target} PRIVATE
            "-sNODERAWFS=1"
            "-sEXIT_RUNTIME=1"
            "-sALLOW_MEMORY_GROWTH=1"
        )
    endif()
endfunction()

# Apply link options for the browser viewer build (Emscripten only). Use this
# *instead* of nodehammer_apply_emscripten_exe_options when building the viewer:
# NODERAWFS is incompatible with browsers, so we deliberately do not set it.
# WebGL2 flags are kept for the SOKOL_GLES3 build; the WebGPU build adds
# -sUSE_WEBGPU=1 separately at its target site.
#
# Output is .js + .wasm (no .html). web/viewer.html is served separately as
# a static page that probes navigator.gpu and dynamically loads the matching
# nodehammer-{gles3,wgpu}.js — see web/viewer.html and Justfile wasm-serve.
function(nodehammer_apply_emscripten_viewer_options target)
    if(NOT EMSCRIPTEN)
        return()
    endif()
    target_link_options(${target} PRIVATE
        "-sEXIT_RUNTIME=0"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sFORCE_FILESYSTEM=1"
        "-sUSE_WEBGL2=1"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        # Fetch API runtime: emscripten_fetch with onsuccess/onprogress
        # callbacks. The viewer uses it to download config/geometry/include
        # assets after main() starts, so imgui can render a progress UI
        # instead of blocking startup on FS.createPreloadedFile.
        "-sFETCH=1"
        # The wasm-exceptions runtime emits JS that calls $stackSave/
        # $stackRestore, which in turn need the C-level emscripten_stack_*
        # helpers from libcompiler_rt. Force-include those JS lib funcs;
        # emcc then auto-pulls the C deps from compiler_rt for us.
        "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$stackSave,$stackRestore"
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".js")
endfunction()

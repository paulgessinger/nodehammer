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
# WebGL2 + offscreen framebuffer is what bgfx's GLES path expects. The shell
# file wraps the generated bundle with a canvas + status element.
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
        "-sOFFSCREEN_FRAMEBUFFER=1"
        "--shell-file=${CMAKE_SOURCE_DIR}/web/viewer.html"
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
endfunction()

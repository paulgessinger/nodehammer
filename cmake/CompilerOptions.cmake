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

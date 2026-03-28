# Sets C++23 compiler warnings and optional sanitizers on a target.
# Call nodehammer_set_compiler_options(target) for each target.

function(nodehammer_set_compiler_options target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PUBLIC
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
        target_compile_options(${target} PUBLIC
            /W4
            /permissive-
        )
    endif()

    if(NODEHAMMER_ENABLE_ASAN)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PUBLIC -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PUBLIC -fsanitize=address)
        else()
            message(WARNING "ASAN requested but compiler is not Clang/GCC; ignoring.")
        endif()
    endif()
endfunction()

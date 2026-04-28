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
        # Pre-allocate enough heap to fit the IBL bake (~16 MB) plus the
        # MEMFS-resident config/geometry, so the bake doesn't trigger wasm
        # memory growth while emscripten_fetch callbacks are still writing
        # files. Heap growth interleaved with MEMFS writes was producing
        # intermittent "No such file" / short-write failures.
        "-sINITIAL_MEMORY=64MB"
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
        # Upload glue: the file-picker EM_JS shim and the drag-and-drop
        # FileReader path both call back into wasm at Module._nh_viewer_*
        # with a malloc'd byte buffer. Default emscripten only exports
        # _main, so opt _malloc / _free in explicitly. HEAPU8 is also
        # opt-in under modern emscripten and is needed to copy the JS
        # bytes into the wasm heap before handing off to the C++ side.
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_nh_viewer_begin_upload_batch,_nh_viewer_add_upload,_nh_viewer_end_upload_batch,_nh_viewer_start"
        # ccall is opt-in under modern emscripten and is needed by the JS
        # shell to invoke nh_viewer_start with a JSON string after runtime
        # init (auto-marshals std::string).
        "-sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall"
        # The wasm-exceptions runtime emits JS that calls $stackSave/
        # $stackRestore, which in turn need the C-level emscripten_stack_*
        # helpers from libcompiler_rt. Force-include those JS lib funcs;
        # emcc then auto-pulls the C deps from compiler_rt for us.
        "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$stackSave,$stackRestore"
    )
    # Size-focused flags for Release wasm only. RelWithDebInfo keeps assertions
    # and debug info so browser stack traces stay readable; Release strips
    # everything and runs Closure on the JS glue. Pair with -Oz / LTO compile
    # flags applied below.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${target} PRIVATE -Oz -flto)
        target_link_options(${target} PRIVATE
            "-Oz"
            "-flto"
            "-sASSERTIONS=0"
            "--closure=1"
        )
    endif()
    set_target_properties(${target} PROPERTIES SUFFIX ".js")
endfunction()

cmake_minimum_required(VERSION 3.25)

# Derive version from the nearest git tag (vMAJOR.MINOR.PATCH).
# On an exact tag the version is clean (e.g. "1.2.3"); between tags it carries
# the commit distance and hash (e.g. "1.2.3-7-gabcdef").
include(cmake/Version.cmake)

project(nodehammer
    VERSION "${NODEHAMMER_VERSION_MAJOR}.${NODEHAMMER_VERSION_MINOR}.${NODEHAMMER_VERSION_PATCH}"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckIncludeFileCXX)
check_include_file_cxx(print NODEHAMMER_HAS_STD_PRINT)
if(NOT NODEHAMMER_HAS_STD_PRINT)
    message(FATAL_ERROR
        "nodehammer requires a C++23 standard library with <print>, but "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
        "${CMAKE_CXX_COMPILER} cannot find it. "
        "Use GCC >= 14, Clang >= 18 with libc++ >= 18, or MSVC >= 19.37.")
endif()

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Emscripten: compile and link all in-tree code (including FetchContent'd
# manifold/clipper2) with native wasm exceptions. Must be set BEFORE
# Dependencies.cmake so FetchContent targets inherit it. The matching conan
# profile flags cover conan-built deps (catch2 etc.) — the two must agree.
if(EMSCRIPTEN)
    # Native wasm exceptions. Modern emscripten (>= 4.0) defaults to the
    # try_table/throw_ref instructions; the old WASM_LEGACY_EXCEPTIONS knob has
    # been removed, so just request -fwasm-exceptions and rely on the default.
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions)
endif()

# ── Options ───────────────────────────────────────────────────────────────────
# Declared before Dependencies.cmake so dep fetching can gate on them
# (the viewer pulls sokol/Dear ImGui only when NODEHAMMER_WITH_VIEWER is ON).
option(NODEHAMMER_WITH_TGEO     "Enable TGeo/ROOT importer"                            OFF)
option(NODEHAMMER_WITH_DD4HEP   "Enable DD4hep importer"                               OFF)
option(NODEHAMMER_WITH_GEANT4   "Enable GDML/Geant4 importer"                          OFF)
option(NODEHAMMER_WITH_VIEWER   "Build the sokol/Dear ImGui interactive viewer"        OFF)
option(NODEHAMMER_VIEWER_NATIVE_DIALOG "Use the native file dialog (NFD); OFF compiles a no-op picker and drops the GTK/DBus dependency" ON)
option(NODEHAMMER_ENABLE_ASAN   "Enable AddressSanitizer"                              OFF)
option(NODEHAMMER_WERROR        "Treat project warnings as errors"                     OFF)
option(NODEHAMMER_BUILD_TESTS   "Build the unit-test binary"                           OFF)
option(NODEHAMMER_BUILD_PYTHON  "Build the nanobind Python extension module"           OFF)

# Everything the extension module links has to be relocatable, and the static
# libs are built as their own targets. Set this before any target is defined.
if(NODEHAMMER_BUILD_PYTHON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
option(NODEHAMMER_PROFILING     "Preserve symbols and frame pointers for sampling profilers" OFF)
option(NODEHAMMER_SHADER_STRICT "Validate cross-compiled HLSL/WGSL shaders at build time (consumed in cmake/Sokol.cmake)" ON)

include(cmake/Dependencies.cmake)
include(cmake/CompilerOptions.cmake)

# ── FlatBuffers code generation ──────────────────────────────────────────────
set(NH_FBS_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/semantic.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/render.fbs
)
set(NH_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers)

# Conan tool_requires exposes flatbuffers::flatc; FetchContent builds a plain
# flatc target; a system install is on PATH. Resolve to whichever is present.
if(TARGET flatbuffers::flatc)
    set(NH_FLATC flatbuffers::flatc)
elseif(TARGET flatc)
    set(NH_FLATC flatc)
else()
    find_program(NH_FLATC_EXE flatc REQUIRED)
    set(NH_FLATC "${NH_FLATC_EXE}")
endif()

set(NH_FBS_GENERATED_HEADERS "")
foreach(schema ${NH_FBS_SCHEMAS})
    get_filename_component(stem ${schema} NAME_WE)
    set(out ${NH_FBS_GENERATED_DIR}/${stem}_generated.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NH_FBS_GENERATED_DIR}
        COMMAND ${NH_FLATC} --cpp -o ${NH_FBS_GENERATED_DIR} ${schema}
        DEPENDS ${schema} ${NH_FLATC}
        COMMENT "Generating FlatBuffers C++ header from ${stem}.fbs"
    )
    list(APPEND NH_FBS_GENERATED_HEADERS ${out})
endforeach()
add_custom_target(flatbuffers_generate DEPENDS ${NH_FBS_GENERATED_HEADERS})

# Profiling flags are additive on top of any build type (Release, RelWithDebInfo,
# Debug). Same interface across native and wasm; the flags differ per toolchain:
# Clang/GCC get -g + frame pointers; MSVC gets /Zi + /Oy-; emscripten gets
# --profiling-funcs on top so wasm function names survive the linker.
if(NODEHAMMER_PROFILING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-g -fno-omit-frame-pointer)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options(/Zi /Oy-)
        add_link_options(/DEBUG)
    endif()
    if(EMSCRIPTEN)
        add_link_options(--profiling-funcs)
    endif()
endif()

# ── Core library ───────────────────────────────────────────────────────────────
# Pipeline core: IR + config + selection + tessellation + importers/exporters
# + scene_build. Deliberately free of CLI11 and the cmd_*.cpp dispatchers so
# the bench harness, the test binary, and the future browser entry point can
# link against it without pulling subcommand registration code (and CLI11's
# header-only template forest) into their closure.
add_library(nodehammer_lib STATIC
    src/ir/semantic.cpp
    src/ir/semantic_json.cpp
    src/ir/render_json.cpp
    src/config/config_loader.cpp
    src/config/config_writer.cpp
    src/config/config_validator.cpp
    src/config/color_parse.cpp
    src/config/predicate_parser.cpp
    src/ir/synthetic/semantic/importer.cpp
    src/ir/semantic/importer_registry.cpp
    src/ir/json/semantic/importer.cpp
    src/ir/fb/semantic/importer.cpp
    src/ir/fb/semantic/flatbuffer.cpp
    src/ir/fb/render/flatbuffer.cpp
    src/selection/predicate.cpp
    src/selection/selector.cpp
    src/tessellation/primitive_tessellator.cpp
    src/tessellation/tessellation_pass.cpp
    src/tessellation/wedge_cut.cpp
    src/tessellation/build_pipeline.cpp
    src/ir/gltf/render/exporter.cpp
    src/ir/obj/render/exporter.cpp
    src/ir/render/exporter_registry.cpp
    src/ir/json/semantic/exporter.cpp
    src/ir/fb/semantic/exporter.cpp
    src/ir/semantic/exporter_registry.cpp
    src/detail/markup.cpp
    src/scene_build.cpp
    src/viewer/app_state.cpp
    src/viewer/camera.cpp
    # Headless, GPU-free viewer controller: the adaptive render-scale state
    # machine. Lives in nodehammer_lib (no sokol deps, like camera.cpp) so the
    # unit tests can link it without the viewer.
    src/viewer/dynamic_render_scale.cpp
    # ProjectFs-based build orchestration. The bag (in-memory key→bytes
    # store) and BuildSession (resolve→parse→import driver) live under
    # src/viewer/ for organisational reasons but have no sokol/GUI deps,
    # so they belong in nodehammer_lib alongside scene_build.cpp — both
    # the viewer and the headless `buildSceneFromPaths` shim drive their
    # builds through this layer.
    src/viewer/bag_project_fs.cpp
    src/viewer/build_session.cpp
    # Archive-internal project manifest (nodehammer.toml [project]) parser — pure
    # toml++, no GUI deps, so it lives in the lib and the unit tests link it.
    src/viewer/project_manifest.cpp
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nodehammer/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp
    @ONLY
)

target_include_directories(nodehammer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(nodehammer_lib PUBLIC
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    glm::glm
    TinyGLTF::TinyGLTF
    zstd::libzstd_static
    flatbuffers::flatbuffers
    unordered_dense::unordered_dense
)

add_dependencies(nodehammer_lib flatbuffers_generate)
target_include_directories(nodehammer_lib SYSTEM PUBLIC ${NH_FBS_GENERATED_DIR})

nodehammer_set_compiler_options(nodehammer_lib)

# ── CLI library ────────────────────────────────────────────────────────────────
# Subcommand dispatchers + CLI11 glue. Native-only: the wasm viewer ships
# its own C-export bootstrap (src/web/viewer_main.cpp) and the CI bench is
# a separate exe under tests/. Nothing in the wasm closure links this lib.
add_library(nodehammer_cli STATIC
    src/cli/run.cpp
    src/cli/pager.cpp
    src/cli/cmd_convert.cpp
    src/cli/cmd_inspect.cpp
    src/cli/cmd_validate_config.cpp
    src/cli/cmd_config_flatten.cpp
    src/cli/cmd_dump_semantic.cpp
    src/cli/cmd_dump_render.cpp
)

target_link_libraries(nodehammer_cli PUBLIC
    nodehammer_lib
    CLI11::CLI11
)

nodehammer_set_compiler_options(nodehammer_cli)

# ── Lua config front-end (native-only) ──────────────────────────────────────────
# The `config-lua` CLI command evaluates a Lua config script into an NHConfig and
# emits flattened TOML (Option A of docs/config-scripting-lua.md). lua + sol2 are
# deliberately kept out of nodehammer_lib — and therefore out of the Emscripten
# closure — so the interpreter lives in this dedicated library, linked only by the
# CLI and the unit tests. Skipped entirely under Emscripten.
if(NOT EMSCRIPTEN)
    add_library(nodehammer_lua STATIC src/lua/lua_config.cpp)
    target_link_libraries(nodehammer_lua PUBLIC nodehammer_lib lua::lua sol2::sol2)
    nodehammer_set_compiler_options(nodehammer_lua)
    # NB: lua_config.cpp silences -Wconversion/-Wsign-conversion in-source via a
    # scoped #pragma (sol2's proxy conversions are not -Wconversion clean). It is
    # kept in the TU rather than here so it can't leak to other sources.

    # Compile the subcommand into the native CLI and expose NH_WITH_LUA. The
    # define is PUBLIC so the executables that compile src/cli/main.cpp (the
    # headless `nodehammer` and the native viewer) see it and register the
    # subcommand via #ifdef.
    target_sources(nodehammer_cli PRIVATE src/cli/cmd_config_lua.cpp)
    target_link_libraries(nodehammer_cli PUBLIC nodehammer_lua)
    target_compile_definitions(nodehammer_cli PUBLIC NH_WITH_LUA=1)
endif()

# ── Optional backends ──────────────────────────────────────────────────────────
if(NODEHAMMER_WITH_TGEO OR NODEHAMMER_WITH_DD4HEP)
    find_package(ROOT REQUIRED COMPONENTS Geom)
    target_sources(nodehammer_lib PRIVATE src/ir/tgeo/semantic/shape_dispatch.cpp)
    target_link_libraries(nodehammer_lib PUBLIC ROOT::Geom)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_TGEO=1)
endif()

if(NODEHAMMER_WITH_TGEO)
    target_sources(nodehammer_lib PRIVATE src/ir/tgeo/semantic/importer.cpp)
endif()

if(NODEHAMMER_WITH_DD4HEP)
    find_package(DD4hep REQUIRED)
    target_sources(nodehammer_lib PRIVATE src/ir/dd4hep/semantic/importer.cpp)
    target_link_libraries(nodehammer_lib PUBLIC DD4hep::DDCore)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_DD4HEP=1)
endif()

if(NODEHAMMER_WITH_GEANT4)
    find_package(Geant4 REQUIRED)
    target_link_libraries(nodehammer_lib PUBLIC ${Geant4_LIBRARIES})
    target_include_directories(nodehammer_lib PUBLIC ${Geant4_INCLUDE_DIRS})
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_GEANT4=1)
endif()

target_sources(nodehammer_lib PRIVATE src/tessellation/boolean_tessellator.cpp)
target_link_libraries(nodehammer_lib PUBLIC manifold::manifold)

# FilesystemProjectFs mounts a real on-disk directory; the web sandbox
# has no equivalent (no recursive_directory_iterator, no readable host
# fs without NODERAWFS), so the source is native-only. It lives in
# nodehammer_lib alongside bag/build_session because future
# `buildSceneFromPaths` callers will want to mount the geometry's
# parent directory in CLI mode.
#
# NativeBagProjectFs (strategy doc step 3) wraps FilesystemProjectFs
# pointed at a process-owned tmp dir. Native-only for the same reason
# the underlying FS backend is.
#
# The in-memory BagProjectFs stays in nodehammer_lib for both platforms:
# the web empty project is now an ArchiveProjectFs working set (§0 reshape
# / R1) so the web *runtime* no longer references BagProjectFs (static-lib
# dead-strip keeps it out of the wasm viewer), but the native CLI
# (cmd_viewer) and the unit tests on both platforms still use it.
if(NOT EMSCRIPTEN)
    target_sources(nodehammer_lib PRIVATE
        src/viewer/filesystem_project_fs.cpp
        src/viewer/native_bag_project_fs.cpp
    )
endif()

# WatchedFilesystemProjectFs (strategy doc step 4) decorates FilesystemProjectFs
# and uses the wtr.watcher Conan dep to bump generation() on disk changes.
# Viewer-gated like platform_folders (the dep is only required when
# `-o '&:viewer=True'`), native-only like the FS backend it wraps. PUBLIC link
# so the Apple CoreServices/CoreFoundation frameworks the recipe propagates
# reach the final executables.
if(NOT EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    target_sources(nodehammer_lib PRIVATE
        src/viewer/watched_filesystem_project_fs.cpp)
    find_package(watcher CONFIG REQUIRED)
    target_link_libraries(nodehammer_lib PUBLIC watcher::watcher)
endif()

# ZipWorkingSet (strategy doc step 5) + ArchiveProjectFs (step 6) + archive
# export (step 7): ZIP-backed read/override/serialize substrate and the archive
# backend. Archive mode is cross-platform — only the POSIX save internals are
# native-only (guarded inside the .cpp) — so these are viewer-gated but *not*
# native-only; miniz builds for Emscripten too, and the web bag (step 8) reuses
# the same substrate.
if(NODEHAMMER_WITH_VIEWER)
    target_sources(nodehammer_lib PRIVATE
        src/viewer/zip_working_set.cpp
        src/viewer/archive_project_fs.cpp
        src/viewer/archive_export.cpp)
    find_package(miniz CONFIG REQUIRED)
    target_link_libraries(nodehammer_lib PUBLIC miniz::miniz)
endif()

# ── Optional viewer (sokol + Dear ImGui) ──────────────────────────────────────
# When the viewer is on, we build a per-backend sokol STATIC lib + a viewer
# executable per backend. Native picks the right backend for the host
# (Metal on Apple, GLCORE on Linux/Windows). Emscripten builds BOTH GLES3
# and WGPU executables from a single configure so the wasm shell can pick
# at runtime via navigator.gpu — see web/viewer.html.
if(NODEHAMMER_WITH_VIEWER)
    # Defines nodehammer_shaders custom target + NH_SHADER_INCLUDE_DIR.
    add_subdirectory(shaders)
    # Defines nh_add_viewer_lib(name sokol_lib).
    add_subdirectory(src/viewer)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_VIEWER=1)

    # Compose one viewer executable. The native build keeps the CLI
    # dispatcher (`nodehammer viewer …` alongside the other subcommands);
    # the wasm build skips CLI11 entirely and uses src/web/viewer_main.cpp,
    # which exposes a single `nh_viewer_start(opts_json)` C export driven
    # by web/viewer.html.
    function(nh_add_viewer_exe name viewer_lib)
        if(EMSCRIPTEN)
            add_executable(${name} src/web/viewer_main.cpp)
            target_link_libraries(${name} PRIVATE ${viewer_lib})
            nodehammer_apply_emscripten_viewer_options(${name})
        else()
            add_executable(${name} src/cli/main.cpp src/cli/cmd_viewer.cpp)
            target_link_libraries(${name} PRIVATE nodehammer_cli ${viewer_lib})
            nodehammer_apply_emscripten_exe_options(${name})
        endif()
        nodehammer_set_compiler_options(${name})
    endfunction()

    if(EMSCRIPTEN)
        # Two backend libs + two viewer libs + two executables from one
        # configure. The viewer source list is compiled twice — once per
        # backend — because the sokol headers' visible struct surface
        # depends on the SOKOL_<backend> define.
        nh_add_sokol_lib(sokol_gles3 SOKOL_GLES3)
        nh_add_sokol_lib(sokol_wgpu  SOKOL_WGPU)
        # WGPU backend needs <webgpu/webgpu.h> at compile time AND the
        # WebGPU runtime polyfill at link time. --use-port=emdawnwebgpu
        # covers both (emcc replaced the old -sUSE_WEBGPU=1 flag with this
        # port). PUBLIC on the lib so the include flag also reaches the
        # viewer TUs compiled into the per-backend viewer lib.
        target_compile_options(sokol_wgpu PUBLIC "--use-port=emdawnwebgpu")
        nh_add_viewer_lib(nodehammer_viewer_gles3 sokol_gles3 SOKOL_GLES3)
        nh_add_viewer_lib(nodehammer_viewer_wgpu  sokol_wgpu  SOKOL_WGPU)
        nh_add_viewer_exe(nodehammer-gles3 nodehammer_viewer_gles3)
        nh_add_viewer_exe(nodehammer-wgpu  nodehammer_viewer_wgpu)
        target_link_options(nodehammer-wgpu PRIVATE "--use-port=emdawnwebgpu")

        # Headless compute module for the viewer's Web Worker: tessellation +
        # wedge cut off the main thread, no sokol/GL. Links the pipeline core
        # only (nodehammer_lib), so it stays small. Driven by
        # src/web/compute_worker.js — see nh_compute_build.
        add_executable(nodehammer-compute src/web/compute_worker_main.cpp)
        target_link_libraries(nodehammer-compute PRIVATE nodehammer_lib)
        nodehammer_apply_emscripten_compute_options(nodehammer-compute)
        nodehammer_set_compiler_options(nodehammer-compute)
    else()
        if(APPLE)
            set(_nh_backend SOKOL_METAL)
        elseif(WIN32)
            set(_nh_backend SOKOL_D3D11)
        else()
            set(_nh_backend SOKOL_GLCORE)
        endif()
        nh_add_sokol_lib(sokol_native ${_nh_backend})
        nh_add_viewer_lib(nodehammer_viewer sokol_native ${_nh_backend})
        nh_add_viewer_exe(nodehammer nodehammer_viewer)

        # Standalone dev tool: just Dear ImGui's demo window in a bare sokol
        # window. Links sokol_native + ImGui::ImGui directly — no
        # nodehammer_lib, no scene/render stack.
        add_executable(imgui-kitchen-sink src/tools/imgui_kitchen_sink_main.cpp)
        target_link_libraries(imgui-kitchen-sink PRIVATE sokol_native ImGui::ImGui)
        nodehammer_set_compiler_options(imgui-kitchen-sink)
    endif()
else()
    # Headless build: just the CLI dispatcher, no viewer subcommand.
    add_executable(nodehammer src/cli/main.cpp)
    target_link_libraries(nodehammer PRIVATE nodehammer_cli)
    nodehammer_set_compiler_options(nodehammer)
    nodehammer_apply_emscripten_exe_options(nodehammer)
endif()

# ── Python extension module ────────────────────────────────────────────────────
# The wheel ships exactly one native artifact: this module, with nodehammer_lib
# and nodehammer_cli linked into it statically. There is no separate `nodehammer`
# executable in the wheel — the console_script entry point is Python calling
# `_nodehammer.main(sys.argv)`, which lands in the same `nodehammer::cli::run`
# the native binary's `main` shim calls. One copy of the code, one artifact.
if(NODEHAMMER_BUILD_PYTHON)
    # SKBUILD_SABI_COMPONENT expands to "Development.SABIModule" when
    # scikit-build-core is building a limited-API wheel (wheel.py-api = cp312),
    # and to nothing otherwise. nanobind silently downgrades STABLE_ABI to a
    # version-locked module unless the Python::SABIModule target exists, so
    # omitting this yields a wheel *tagged* abi3 that only imports on the
    # interpreter it was built with.
    find_package(Python 3.10 REQUIRED
        COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT})

    # Conan (`-o '&:python=True'`) is the expected source, like every other
    # dependency. Fall back to the copy inside an installed `nanobind` Python
    # package — which lives in site-packages rather than on CMAKE_PREFIX_PATH,
    # hence the interpreter probe — so a PEP 517 build with nanobind in
    # build-system.requires also works without a Conan profile.
    find_package(nanobind CONFIG QUIET)
    if(NOT nanobind_FOUND)
        execute_process(
            COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            OUTPUT_VARIABLE nanobind_ROOT
            RESULT_VARIABLE _nb_probe)
        if(NOT _nb_probe EQUAL 0)
            message(FATAL_ERROR
                "nanobind not found. Either configure with Conan "
                "(`conan install . -o '&:python=True'`) or `pip install nanobind`.")
        endif()
        find_package(nanobind CONFIG REQUIRED)
    endif()

    # STABLE_ABI builds a cp312-abi3 module on Python >= 3.12 (silently ignored
    # below that), so one wheel per platform covers every later interpreter
    # instead of one wheel per Python version.
    nanobind_add_module(_nodehammer STABLE_ABI src/python/bindings.cpp)
    target_link_libraries(_nodehammer PRIVATE nodehammer_cli)
    nodehammer_set_compiler_options(_nodehammer)

    # A Python extension module shares its process with every other extension
    # module. Exporting our statically-linked deps (zstd, flatbuffers, manifold,
    # lua, ...) invites symbol interposition against a different build of the
    # same library in some unrelated wheel — a crash with no useful stack.
    #
    # Visibility presets on our own targets are *not* sufficient: most of the
    # leakage is plain C symbols (ZSTD_*, lua_*) from Conan-built archives we
    # don't compile. Measured on macOS before this block: 1299 exported symbols,
    # 864 of them third-party. So pin the export list at link time instead —
    # PyInit__nodehammer is the only symbol an extension module owes anyone.
    set_target_properties(nodehammer_lib nodehammer_cli PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(APPLE)
        # Mach-O prefixes C symbols with an underscore.
        target_link_options(_nodehammer PRIVATE
            "LINKER:-exported_symbol,_PyInit__nodehammer")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # --exclude-libs hides symbols pulled from static archives; the version
        # script then pins what remains. Belt and braces, because a dep linked
        # as a shared object would slip past --exclude-libs.
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nodehammer_python.map"
            "{ global: PyInit__nodehammer; local: *; };\n")
        target_link_options(_nodehammer PRIVATE
            "LINKER:--exclude-libs,ALL"
            "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/nodehammer_python.map")
    endif()

    # Installed under its own component so scikit-build-core can take just this
    # and leave the native `nodehammer` executable / headers out of the wheel.
    install(TARGETS _nodehammer
        LIBRARY DESTINATION nodehammer
        COMPONENT python)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────────
if(NODEHAMMER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

# Native: single nodehammer executable. Emscripten + viewer: per-backend
# wasm bundles named nodehammer-{gles3,wgpu}.{js,wasm}. Emscripten + no
# viewer: headless nodehammer (just the .js + .wasm).
if(EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    set(NH_INSTALL_TARGETS nodehammer-gles3 nodehammer-wgpu nodehammer-compute)
else()
    set(NH_INSTALL_TARGETS nodehammer)
endif()

install(TARGETS ${NH_INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Emscripten produces a sidecar .wasm next to each .js loader; CMake's
# install(TARGETS RUNTIME) only picks up the primary output, so install
# the blobs explicitly.
if(EMSCRIPTEN)
    foreach(_t ${NH_INSTALL_TARGETS})
        install(FILES "$<TARGET_FILE_DIR:${_t}>/${_t}.wasm"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endforeach()
endif()

install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}"
)

# Reads the manifest produced by conanfile.py's generate(), assembles
# THIRD-PARTY-NOTICES.txt at configure time, and installs it + the staged
# per-dep folders. Silent no-op when no manifest is present (non-Conan path).
include(cmake/ThirdPartyLicenses.cmake)

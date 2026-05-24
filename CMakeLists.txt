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
# (the viewer pulls bgfx/SDL3/ImGui only when NODEHAMMER_WITH_VIEWER is ON).
option(NODEHAMMER_WITH_TGEO     "Enable TGeo/ROOT importer"                            OFF)
option(NODEHAMMER_WITH_DD4HEP   "Enable DD4hep importer"                               OFF)
option(NODEHAMMER_WITH_GEANT4   "Enable GDML/Geant4 importer"                          OFF)
option(NODEHAMMER_WITH_GEOMODEL "Enable GeoModel importer"                             OFF)
option(NODEHAMMER_WITH_VIEWER   "Build the sokol/Dear ImGui interactive viewer"        OFF)
option(NODEHAMMER_ENABLE_ASAN   "Enable AddressSanitizer"                              OFF)
option(NODEHAMMER_WERROR        "Treat project warnings as errors"                     OFF)
option(NODEHAMMER_BUILD_TESTS   "Build the unit-test binary"                           OFF)
option(NODEHAMMER_PROFILING     "Preserve symbols and frame pointers for sampling profilers" OFF)

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
    src/config/config_loader.cpp
    src/config/config_writer.cpp
    src/config/config_validator.cpp
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
    # ProjectFs-based build orchestration. The bag (in-memory key→bytes
    # store) and BuildSession (resolve→parse→import driver) live under
    # src/viewer/ for organisational reasons but have no sokol/GUI deps,
    # so they belong in nodehammer_lib alongside scene_build.cpp — both
    # the viewer and the headless `buildSceneFromPaths` shim drive their
    # builds through this layer.
    src/viewer/bag_project_fs.cpp
    src/viewer/build_session.cpp
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
# the underlying FS backend is — and the in-memory BagProjectFs stays
# as the web bag until WebBagProjectFs lands (step 8).
if(NOT EMSCRIPTEN)
    target_sources(nodehammer_lib PRIVATE
        src/viewer/filesystem_project_fs.cpp
        src/viewer/native_bag_project_fs.cpp
    )
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
        nh_add_viewer_lib(nodehammer_viewer_gles3 sokol_gles3)
        nh_add_viewer_lib(nodehammer_viewer_wgpu  sokol_wgpu)
        nh_add_viewer_exe(nodehammer-gles3 nodehammer_viewer_gles3)
        nh_add_viewer_exe(nodehammer-wgpu  nodehammer_viewer_wgpu)
        target_link_options(nodehammer-wgpu PRIVATE "--use-port=emdawnwebgpu")
    else()
        if(APPLE)
            set(_nh_backend SOKOL_METAL)
        elseif(WIN32)
            set(_nh_backend SOKOL_D3D11)
        else()
            set(_nh_backend SOKOL_GLCORE)
        endif()
        nh_add_sokol_lib(sokol_native ${_nh_backend})
        nh_add_viewer_lib(nodehammer_viewer sokol_native)
        nh_add_viewer_exe(nodehammer nodehammer_viewer)
    endif()
else()
    # Headless build: just the CLI dispatcher, no viewer subcommand.
    add_executable(nodehammer src/cli/main.cpp)
    target_link_libraries(nodehammer PRIVATE nodehammer_cli)
    nodehammer_set_compiler_options(nodehammer)
    nodehammer_apply_emscripten_exe_options(nodehammer)
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
    set(NH_INSTALL_TARGETS nodehammer-gles3 nodehammer-wgpu)
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

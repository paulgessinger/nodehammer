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

# Every object that can end up inside a shared object must be position
# independent: the planned installed shared library, and — sooner — the Python
# extension module, which links the *static* library into a .so. CMake adds
# -fPIC automatically for SHARED/MODULE targets but never for STATIC ones, so
# the archive is the case that needs this. Set here, before Dependencies.cmake,
# so FetchContent subprojects inherit it too (flatbuffers is the one that does
# not set it itself). It expands to nothing for MSVC, and makes native
# executables PIE, which is already the default on most distributions.
# NODEHAMMER_PIC_CHECK (below) is what keeps this honest.
#
# Skipped under Emscripten: there is no shared object to build there, and emcc
# warns on -fPIC unless dynamic linking is enabled — which the wasm CI job,
# building with NODEHAMMER_WERROR=ON, would take as an error.
if(NOT EMSCRIPTEN)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

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
option(NODEHAMMER_PROFILING     "Preserve symbols and frame pointers for sampling profilers" OFF)
option(NODEHAMMER_SHADER_STRICT "Validate cross-compiled HLSL/WGSL shaders at build time (consumed in cmake/Sokol.cmake)" ON)
option(NODEHAMMER_PIC_CHECK     "Link a probe shared module against the static library to prove every object is position-independent (consumed in cmake/PicProbe.cmake)" OFF)
option(NODEHAMMER_BUILD_SHARED  "Also build and install the shared library + public headers + CMake package config" OFF)

include(cmake/Dependencies.cmake)
include(cmake/CompilerOptions.cmake)

# Included early: the core-library helper below bakes CMAKE_INSTALL_INCLUDEDIR
# into the shared target's INSTALL_INTERFACE include path.
include(GNUInstallDirs)

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

# ── Core library configuration ────────────────────────────────────────────────
# Pipeline core: IR + config + selection + tessellation + importers/exporters
# + scene_build. Deliberately free of CLI11 and the cmd_*.cpp dispatchers so
# the bench harness, the test binary, and the future browser entry point can
# link against it without pulling subcommand registration code (and CLI11's
# header-only template forest) into their closure.
#
# Accumulated into variables rather than written straight onto a target because
# the core is built *twice* in a packaging build: once as the static archive
# every in-tree target links, and once as the installed shared library. The two
# cannot share objects — __declspec(dllexport) is codegen-affecting, so DLL
# objects are not valid inside a static library, which rules out the OBJECT
# library trick on Windows — so the configuration is what gets shared instead,
# and nh_add_core_library() below is the only place either target is described.
# Anything appended here lands in both variants by construction; that symmetry
# is the point, since a source present in one and absent from the other would
# surface as a link error only in the packaging job.
set(NH_CORE_SOURCES
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
    src/export_resolve.cpp
    src/version.cpp
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

set(NH_CORE_DEPS
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    glm::glm
    TinyGLTF::TinyGLTF
    zstd::libzstd_static
    flatbuffers::flatbuffers
    unordered_dense::unordered_dense
)

# nlohmann stays PUBLIC here, and #41's "nlohmann can finally go PRIVATE" row is
# satisfied by a different mechanism than the one that row anticipated.
#
# Two reasons the dep-scope flip is not the lever it looks like. First, it would
# change nothing: TinyGLTF::TinyGLTF lists nlohmann_json::nlohmann_json in its
# own INTERFACE_LINK_LIBRARIES, so nlohmann reaches every in-tree target through
# tinygltf whatever the core declares — verified by dropping it from this list
# and rebuilding, which compiles clean. Second, it would be a misdescription:
# src/ir/render.hpp says `using ExtrasMap = nlohmann::json;`, so nlohmann really
# is part of the archive's interface as far as in-tree consumers are concerned,
# and calling it PRIVATE would be a latent lie that only bites the day tinygltf
# stops carrying it.
#
# What the row actually wanted — nlohmann absent from the *installed* interface
# — holds, and is enforced where it belongs: the shared variant takes every
# dependency PRIVATE behind $<BUILD_INTERFACE:>, so the generated export has no
# INTERFACE_LINK_LIBRARIES at all, and ci/shared_consumer builds with nothing
# but nodehammer's own prefix on CMAKE_PREFIX_PATH.
set(NH_CORE_DEFINES "")

# Extra include directories for dependencies that ship variables instead of
# imported targets — Geant4 is the only one today. Everything else propagates
# its includes through the link.
set(NH_CORE_INCLUDE_DIRS "")

# ── Core library: optional pieces ─────────────────────────────────────────────
# Everything below appends to the three lists above, so it reaches both the
# static and the shared variant. find_package() calls stay here rather than
# inside the helper: they are configuration, not target description, and must
# run exactly once.

# Lua config front-end. Compiled into the core rather than a library of its own.
#
# It used to be a separate nodehammer_lua target, which read as the tidy way to
# keep the interpreter out of the wasm closure — but the constraint was only
# ever "no lua under Emscripten", and a gated source satisfies that identically
# (docs/config-scripting-lua.md, and #41 §6). The separate target actively cost
# us three things: `Config::read` dispatching a `.lua` path needs the engine
# *inside* the shared library, which a target that itself links the core cannot
# provide without linking the core into itself; NH_WITH_LUA sat on the CLI where
# the library could not see it, though the library is what has to answer
# `Config::formats()`; and cmake/PicProbe.cmake needed a special case to avoid
# pulling the core archive twice under --whole-archive.
#
# The dependency direction is unchanged: lua_config.hpp includes
# config/config_loader.hpp, never the reverse, so this is a merge downhill.
# `-Wconversion`/`-Wsign-conversion` are silenced by a scoped #pragma inside
# lua_config.cpp (sol2's proxy conversions are not clean under them), kept in
# the TU so it cannot leak to neighbouring sources now that they share a target.
if(NOT EMSCRIPTEN)
    list(APPEND NH_CORE_SOURCES src/lua/lua_config.cpp)
    list(APPEND NH_CORE_DEPS lua::lua sol2::sol2)
    list(APPEND NH_CORE_DEFINES NH_WITH_LUA=1)
endif()

if(NODEHAMMER_WITH_TGEO OR NODEHAMMER_WITH_DD4HEP)
    find_package(ROOT REQUIRED COMPONENTS Geom)
    list(APPEND NH_CORE_SOURCES src/ir/tgeo/semantic/shape_dispatch.cpp)
    list(APPEND NH_CORE_DEPS ROOT::Geom)
    list(APPEND NH_CORE_DEFINES NH_WITH_TGEO=1)
endif()

if(NODEHAMMER_WITH_TGEO)
    list(APPEND NH_CORE_SOURCES src/ir/tgeo/semantic/importer.cpp)
endif()

if(NODEHAMMER_WITH_DD4HEP)
    find_package(DD4hep REQUIRED)
    list(APPEND NH_CORE_SOURCES src/ir/dd4hep/semantic/importer.cpp)
    list(APPEND NH_CORE_DEPS DD4hep::DDCore)
    list(APPEND NH_CORE_DEFINES NH_WITH_DD4HEP=1)
endif()

if(NODEHAMMER_WITH_GEANT4)
    find_package(Geant4 REQUIRED)
    # Geant4 exports plain variables rather than imported targets, so unlike
    # every other backend its include directories do not ride along with the
    # link and have to be named separately.
    list(APPEND NH_CORE_DEPS ${Geant4_LIBRARIES})
    list(APPEND NH_CORE_INCLUDE_DIRS ${Geant4_INCLUDE_DIRS})
    list(APPEND NH_CORE_DEFINES NH_WITH_GEANT4=1)
endif()

list(APPEND NH_CORE_SOURCES src/tessellation/boolean_tessellator.cpp)
list(APPEND NH_CORE_DEPS manifold::manifold)

# FilesystemProjectFs mounts a real on-disk directory; the web sandbox
# has no equivalent (no recursive_directory_iterator, no readable host
# fs without NODERAWFS), so the source is native-only. It lives in the
# core alongside bag/build_session because future `buildSceneFromPaths`
# callers will want to mount the geometry's parent directory in CLI mode.
#
# NativeBagProjectFs (strategy doc step 3) wraps FilesystemProjectFs
# pointed at a process-owned tmp dir. Native-only for the same reason
# the underlying FS backend is.
#
# The in-memory BagProjectFs stays in the core for both platforms: the web
# empty project is now an ArchiveProjectFs working set (§0 reshape / R1) so
# the web *runtime* no longer references BagProjectFs (static-lib dead-strip
# keeps it out of the wasm viewer), but the native CLI (cmd_viewer) and the
# unit tests on both platforms still use it.
if(NOT EMSCRIPTEN)
    list(APPEND NH_CORE_SOURCES
        src/viewer/filesystem_project_fs.cpp
        src/viewer/native_bag_project_fs.cpp
    )
endif()

# WatchedFilesystemProjectFs (strategy doc step 4) decorates FilesystemProjectFs
# and uses the wtr.watcher Conan dep to bump generation() on disk changes.
# Viewer-gated like platform_folders (the dep is only required when
# `-o '&:viewer=True'`), native-only like the FS backend it wraps. The Apple
# CoreServices/CoreFoundation frameworks the recipe propagates have to reach
# the final executables, which they do via the static variant's PUBLIC link.
if(NOT EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    list(APPEND NH_CORE_SOURCES src/viewer/watched_filesystem_project_fs.cpp)
    find_package(watcher CONFIG REQUIRED)
    list(APPEND NH_CORE_DEPS watcher::watcher)
endif()

# ZipWorkingSet (strategy doc step 5) + ArchiveProjectFs (step 6) + archive
# export (step 7): ZIP-backed read/override/serialize substrate and the archive
# backend. Archive mode is cross-platform — only the POSIX save internals are
# native-only (guarded inside the .cpp) — so these are viewer-gated but *not*
# native-only; miniz builds for Emscripten too, and the web bag (step 8) reuses
# the same substrate.
if(NODEHAMMER_WITH_VIEWER)
    list(APPEND NH_CORE_SOURCES
        src/viewer/zip_working_set.cpp
        src/viewer/archive_project_fs.cpp
        src/viewer/archive_export.cpp
    )
    find_package(miniz CONFIG REQUIRED)
    list(APPEND NH_CORE_DEPS miniz::miniz)
    list(APPEND NH_CORE_DEFINES NH_WITH_VIEWER=1)
endif()

# ── Core library targets ──────────────────────────────────────────────────────
# Describe one core-library variant from the lists above.
#
# `depScope` is the whole difference between the two, and it is not cosmetic.
# The static archive is never installed, so its dependencies are PUBLIC: in-tree
# targets include internal headers that include toml++, glm and nlohmann
# directly, and they reach them through this link. The shared library is the one
# that ships, and by §10 it absorbs its static dependencies whole — so they are
# PRIVATE *and* wrapped in $<BUILD_INTERFACE:>, which drops them from the
# generated export set. That is what spares an external consumer a
# find_dependency() for zstd/flatbuffers/manifold it has no way to satisfy and
# no reason to know about.
function(nh_add_core_library target type depScope)
    add_library(${target} ${type} ${NH_CORE_SOURCES})

    # `include/` holds the public headers; every internal header lives next to
    # its source under `src/` and is included as <ir/semantic.hpp>,
    # <config/...>, etc. The split is physical, so nothing internal *can* leak
    # into an install: the BUILD_INTERFACE genex keeps src/ on the include path
    # for every in-tree target (cli, viewer, tests) while dropping it from the
    # exported interface, which carries only the installed include dir.
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    if(NH_CORE_INCLUDE_DIRS)
        target_include_directories(${target} SYSTEM ${depScope} ${NH_CORE_INCLUDE_DIRS})
    endif()

    if(depScope STREQUAL "PRIVATE")
        foreach(dep IN LISTS NH_CORE_DEPS)
            target_link_libraries(${target} PRIVATE "$<BUILD_INTERFACE:${dep}>")
        endforeach()
    else()
        target_link_libraries(${target} ${depScope} ${NH_CORE_DEPS})
    endif()

    # Same scope split, and for a sharper reason than the deps. In-tree code
    # gates on NH_WITH_TGEO / NH_WITH_LUA / NH_WITH_VIEWER, so the archive
    # propagates them. The installed library must not: by §5 no public header
    # contains an #if, so a consumer has no use for them, and pushing
    # NH_WITH_LUA=1 into someone else's translation units is a collision
    # waiting to happen. PRIVATE here is what keeps INTERFACE_COMPILE_DEFINITIONS
    # empty in the generated export, which is in turn what makes the installed
    # headers self-describing — the same property §5 is protecting.
    target_compile_definitions(${target} ${depScope} ${NH_CORE_DEFINES})

    # The one usage requirement that *is* real for a consumer: these headers
    # need C++23. CMAKE_CXX_STANDARD is a build-tree setting and does not
    # propagate through an export, so state it on the target.
    target_compile_features(${target} PUBLIC cxx_std_23)

    add_dependencies(${target} flatbuffers_generate)
    target_include_directories(${target} SYSTEM PRIVATE ${NH_FBS_GENERATED_DIR})

    nodehammer_set_compiler_options(${target})
    nodehammer_set_visibility(${target})
endfunction()

nh_add_core_library(nodehammer_lib STATIC PUBLIC)

# NH_STATIC collapses NH_API to nothing (include/nodehammer/api.hpp). PUBLIC so
# every in-tree consumer — CLI, viewer, tests — inherits it and no target has to
# remember; without it they would see the dllimport spelling on Windows and fail
# to link against the archive.
target_compile_definitions(nodehammer_lib PUBLIC NH_STATIC)

# The generated FlatBuffers headers are PRIVATE to the core in the helper above,
# but in-tree targets that include <ir/fb/...> transitively need them too, and
# only the static variant has in-tree consumers.
target_include_directories(nodehammer_lib SYSTEM PUBLIC ${NH_FBS_GENERATED_DIR})

if(NODEHAMMER_BUILD_SHARED)
    if(EMSCRIPTEN)
        message(FATAL_ERROR
            "NODEHAMMER_BUILD_SHARED is not meaningful under Emscripten: there "
            "is no shared object to build, and the wasm targets are statically "
            "linked executables whose exports are named by the link flags.")
    endif()

    nh_add_core_library(nodehammer_shared SHARED PRIVATE)

    # NH_EXPORTS selects the dllexport spelling of NH_API while compiling the
    # library itself; PRIVATE, since a consumer must see dllimport instead.
    target_compile_definitions(nodehammer_shared PRIVATE NH_EXPORTS)

    # The exported name is `nodehammer`; the target cannot be, because the CLI
    # executable already owns that target name.
    set_target_properties(nodehammer_shared PROPERTIES
        OUTPUT_NAME nodehammer
        EXPORT_NAME nodehammer
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )

    # Hidden visibility only governs the objects *we* compile. The static
    # dependencies absorbed into this .so were compiled without it, so their
    # symbols are default-visible and would land in the dynamic export table
    # wholesale — zstd, flatbuffers, manifold and friends, re-exported by
    # accident. --exclude-libs,ALL forces every symbol arriving from a static
    # archive to hidden, which is what makes the export table equal the public
    # API rather than merely contain it.
    #
    # --no-undefined then asserts the other half of §10's claim: that a shared
    # library resolves its private dependencies internally, so an external
    # consumer needs no find_dependency() for them.
    #
    # ELF-only, like cmake/PicProbe.cmake and for the same reason: ld64 has no
    # equivalent of either flag (only per-archive -hidden-l), so on macOS the
    # dependencies' symbols do stay visible in the dylib. ci/check_shared_exports.py
    # is correspondingly Linux-only.
    if(UNIX AND NOT APPLE)
        target_link_options(nodehammer_shared PRIVATE
            "LINKER:--exclude-libs,ALL"
            "LINKER:--no-undefined"
        )
    endif()
endif()

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

# The `config-lua` subcommand: evaluates a Lua config script into an NHConfig
# and emits flattened TOML (Option A of docs/config-scripting-lua.md). The
# engine itself is in the core; only the dispatcher is CLI-side, and only
# natively — NH_WITH_LUA comes from the core's PUBLIC defines.
if(NOT EMSCRIPTEN)
    target_sources(nodehammer_cli PRIVATE src/cli/cmd_config_lua.cpp)
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

# ── Tests ──────────────────────────────────────────────────────────────────────
if(NODEHAMMER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Included last: the probe links every library target that can end up inside a
# shared object, so all of them have to exist first.
include(cmake/PicProbe.cmake)

# ── Install ────────────────────────────────────────────────────────────────────
# (GNUInstallDirs is included near the top — the core-library helper needs
# CMAKE_INSTALL_INCLUDEDIR for the shared target's INSTALL_INTERFACE.)

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

# ── Library install: the shared library, the public headers, a package config ──
# Gated on NODEHAMMER_BUILD_SHARED, which is what makes the split load-bearing
# rather than aspirational: the only headers that can reach a consumer are the
# ones under include/nodehammer, because that is the only directory named here.
# There is no manifest of public headers to keep in sync with reality and no
# annotation an internal header could carry by mistake — an internal header is
# unreachable because it lives somewhere else on disk (#41 §1).
if(NODEHAMMER_BUILD_SHARED)
    install(TARGETS nodehammer_shared
        EXPORT nodehammer-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}   # Windows import library
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # Windows DLL
    )

    # FILES_MATCHING "*.hpp" also excludes version.hpp.in, which would otherwise
    # install an uninstantiated template next to the header generated from it.
    install(DIRECTORY include/nodehammer
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nodehammer"
    )

    install(EXPORT nodehammer-targets
        FILE nodehammer-targets.cmake
        NAMESPACE nodehammer::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer"
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nodehammer-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer"
    )
    # SameMinorVersion, not the usual SameMajorVersion: pre-1.0, the major
    # version conveys nothing, and treating every 0.x as interchangeable would
    # promise compatibility across exactly the releases where the API is
    # expected to move. Revisit at 1.0.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config-version.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config-version.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer"
    )
endif()

# Reads the manifest produced by conanfile.py's generate(), assembles
# THIRD-PARTY-NOTICES.txt at configure time, and installs it + the staged
# per-dep folders. Silent no-op when no manifest is present (non-Conan path).
include(cmake/ThirdPartyLicenses.cmake)

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
# library trick on Windows — so the configuration is what gets shared instead.
# Anything appended to *this* list lands in both variants by construction, which
# is where the "describe once" guarantee actually lives. NH_APP_SOURCES below is
# the deliberate exception: sources the static archive gets and the installed
# shared library does not.
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
    # The in-memory bag (key→bytes store) behind ProjectFs. Under src/viewer/ for
    # organisational reasons, but no sokol/GUI deps and part of the project
    # substrate rather than the app — see NH_APP_SOURCES for where that line is.
    src/viewer/bag_project_fs.cpp
    # Archive-internal project manifest (nodehammer.toml [project]) parser — pure
    # toml++, no GUI deps, so it lives in the lib and the unit tests link it.
    src/viewer/project_manifest.cpp
)

# ── Application sources ───────────────────────────────────────────────────────
# Compiled into the static archive that every in-tree target links, and
# deliberately *not* into the installed shared library.
#
# The shared library is the headless pipeline core, which is what the public API
# is: IR, config, selection, tessellation, importers, exporters, and in time the
# ability to read and write a .nhproj. A consumer does not want a camera, a UI
# state machine, or an adaptive render-scale controller, and should not pay
# watcher's Apple CoreServices/CoreFoundation link for a file watcher it never
# calls.
#
# The line is drawn here rather than at `src/viewer/*`, because that directory
# holds two unrelated clusters. The project/VFS/archive stack — project_fs,
# bag_project_fs, filesystem_project_fs, native_bag_project_fs, zip_working_set,
# archive_project_fs, archive_export, project_manifest — reaches only itself plus
# byte_buffer and log_sink, and is the .nhproj capability, so it is core. What
# remains is the app: app_state.hpp names camera/config/render_quality,
# build_session.hpp names log_sink/project_fs. Both point *into* the core cluster
# and never out of it, which is what makes the subtraction safe.
#
# That direction is also what checks it. Since nothing in the core references the
# app set, dropping these from the shared library cannot leave an undefined
# symbol — and if a pipeline source is ever misfiled into this list,
# --no-undefined fails the shared link and names it. The previous arrangement
# built both variants from one list and treated the symmetry as an invariant;
# this is the same guarantee, asserted by the linker rather than by convention.
set(NH_APP_SOURCES
    src/viewer/app_state.cpp
    src/viewer/camera.cpp
    # Headless, GPU-free viewer controller: the adaptive render-scale state
    # machine. No sokol deps (like camera.cpp), so the unit tests can link it
    # without the viewer.
    src/viewer/dynamic_render_scale.cpp
    # Build orchestration (resolve→parse→import driver). Drives builds for the
    # viewer and the headless buildSceneFromPaths shim; app-side for now, and a
    # plausible candidate to promote if step 6 exposes it.
    src/viewer/build_session.cpp
)

# Dependencies and defines that follow NH_APP_SOURCES onto the static archive
# only. Appended to below, alongside their sources.
set(NH_APP_DEPS "")
set(NH_APP_DEFINES "")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nodehammer/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp
    @ONLY
)

# Dependency scope on the *archive*, which is a different question from scope on
# the installed shared library (that one is uniformly PRIVATE, behind
# $<BUILD_INTERFACE:>). The archive is consumed in-tree through internal headers,
# so the test is simply: does an internal header include the dependency's headers
# in a way an out-of-core target sees?
#
# Determined by moving the whole list to PRIVATE and rebuilding until it was
# clean, three iterations — `fatal error` stops at the first bad include, so one
# pass reports only the first offender per TU and reads as a much shorter list
# than the truth.
set(NH_CORE_DEPS
    # src/config/config_ast.hpp, src/ir/render.hpp — `using ExtrasMap = nlohmann::json`
    nlohmann_json::nlohmann_json
    # src/detail/glm_json.hpp, src/viewer/camera.hpp
    glm::glm
    # src/detail/zstd_io.hpp
    zstd::libzstd_static
    # src/ir/semantic.hpp, src/ir/render.hpp
    unordered_dense::unordered_dense
    # src/ir/fb/{semantic,render}/flatbuffer.hpp — flatbuffers::Offset in signatures
    flatbuffers::flatbuffers
)

# Used only from .cpp files in the core, so nothing outside it needs them.
#
# This is also what settles #41's "nlohmann can finally go PRIVATE" row, and it
# settles it the other way. The row assumed nlohmann was over-declared; in fact
# it is one of the few that is *correctly* PUBLIC, because ExtrasMap puts
# nlohmann::json in an internal header's interface. What was over-declared was
# tinygltf — and tinygltf is what made the question hard to answer, since it
# lists nlohmann_json in its own INTERFACE_LINK_LIBRARIES and so handed nlohmann
# to every in-tree target regardless of what this file said. With tinygltf
# PRIVATE that confound is gone: nlohmann's PUBLIC entry is now load-bearing and
# removing it fails the build, which is the property the row wanted to be able
# to check.
#
# The installed side was never affected either way: the shared variant takes
# every dependency PRIVATE behind $<BUILD_INTERFACE:>, so the generated export
# has no INTERFACE_LINK_LIBRARIES at all and ci/shared_consumer builds with
# nothing but nodehammer's own prefix on CMAKE_PREFIX_PATH.
set(NH_CORE_PRIVATE_DEPS
    tomlplusplus::tomlplusplus
    # tests/export/test_gltf_exporter.cpp includes <tiny_gltf.h> to read back what
    # the exporter wrote, and declares the dependency itself — see tests/.
    TinyGLTF::TinyGLTF
)

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
    # lua_config.hpp exposes only config::ConfigResult, so the interpreter and
    # its binding stay inside the core's implementation.
    list(APPEND NH_CORE_PRIVATE_DEPS lua::lua sol2::sol2)
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
# `-o '&:viewer=True'`), native-only like the FS backend it wraps.
#
# App-side, and it is the source that makes the split pay for itself: live
# reload is a viewer feature, and watcher is the one dependency whose recipe
# propagates the Apple CoreServices/CoreFoundation frameworks. Keeping it out of
# the shared library keeps those frameworks off an installed libnodehammer that
# would never call them. Promote it if a consumer ever wants a watching
# ProjectFs — the decorated FilesystemProjectFs it wraps is already core.
if(NOT EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    list(APPEND NH_APP_SOURCES src/viewer/watched_filesystem_project_fs.cpp)
    find_package(watcher CONFIG REQUIRED)
    # PUBLIC on the archive: watched_filesystem_project_fs.hpp and viewer/app.hpp
    # both name wtr types, and the viewer libs include them.
    list(APPEND NH_APP_DEPS watcher::watcher)
endif()

# ZipWorkingSet (strategy doc step 5) + ArchiveProjectFs (step 6) + archive
# export (step 7): ZIP-backed read/override/serialize substrate and the archive
# backend. Archive mode is cross-platform — only the POSIX save internals are
# native-only (guarded inside the .cpp) — so these are viewer-gated but *not*
# native-only; miniz builds for Emscripten too, and the web bag (step 8) reuses
# the same substrate.
#
# Core, not app: this is the .nhproj capability, and the library is expected to
# read and write project archives in its own right rather than only on the
# viewer's behalf. So miniz stays a dependency of the shared library.
#
# It is nevertheless still gated on NODEHAMMER_WITH_VIEWER, which is the one
# remaining respect in which the installed shared library is not invariant to
# that option — a viewer-off build produces a libnodehammer without archive
# support. Removing the gate is what completes the invariance, and it is a
# dependency-baseline decision rather than a packaging one: miniz is requested
# under `if self.options.viewer` in conanfile.py, so ungating here means
# requiring it for every build, wasm included (it does build for Emscripten).
# Deferred until .nhproj is actually part of the public API — at which point the
# change is deleting this condition, not moving files between lists.
#
# NH_WITH_VIEWER goes to the app defines: nothing in the core gates on it (the
# only use in the whole tree is src/cli/main.cpp, reached via the archive's
# PUBLIC propagation), so the shared library has no business carrying it.
if(NODEHAMMER_WITH_VIEWER)
    list(APPEND NH_CORE_SOURCES
        src/viewer/zip_working_set.cpp
        src/viewer/archive_project_fs.cpp
        src/viewer/archive_export.cpp
    )
    find_package(miniz CONFIG REQUIRED)
    # No header mentions miniz; ZipWorkingSet keeps it behind its .cpp.
    list(APPEND NH_CORE_PRIVATE_DEPS miniz::miniz)
    list(APPEND NH_APP_DEFINES NH_WITH_VIEWER=1)
endif()

# ── Core library targets ──────────────────────────────────────────────────────
# Everything both variants need that does not depend on which variant it is.
#
# Deliberately takes no parameter beyond the target. An earlier version of this
# helper took a `depScope` that drove the link scope, the define scope and the
# include scope at once, plus an `if(depScope STREQUAL "PRIVATE")` branch — it
# was a variant discriminator wearing a scope keyword's clothes, and roughly half
# the function body was conditional on it. The genuine "describe once" guarantee
# never lived in the function anyway; it lives in the NH_CORE_* lists, which are
# what make a new source or dependency reach both targets. So the two variants
# are now written out below, and what survives here is the part that is the same
# either way — including nodehammer_set_visibility(), which is the one step
# neither target can afford to skip.
function(nh_core_common target)
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

    # The one usage requirement that *is* real for a consumer: these headers
    # need C++23. CMAKE_CXX_STANDARD is a build-tree setting and does not
    # propagate through an export, so state it on the target.
    target_compile_features(${target} PUBLIC cxx_std_23)

    add_dependencies(${target} flatbuffers_generate)
    target_include_directories(${target} SYSTEM PRIVATE ${NH_FBS_GENERATED_DIR})

    nodehammer_set_compiler_options(${target})
    nodehammer_set_visibility(${target})
endfunction()

# ── Static archive ────────────────────────────────────────────────────────────
# What every in-tree target links — CLI, viewer, tests, benchmarks, and the
# future Python extension. Never installed, so its dependencies are PUBLIC:
# in-tree targets include internal headers that include toml++, glm and nlohmann
# directly, and they reach them through this link. Carries the app sources on
# top of the core.
add_library(nodehammer_lib STATIC ${NH_CORE_SOURCES} ${NH_APP_SOURCES})
nh_core_common(nodehammer_lib)

target_link_libraries(nodehammer_lib PUBLIC ${NH_CORE_DEPS} ${NH_APP_DEPS})
target_link_libraries(nodehammer_lib PRIVATE ${NH_CORE_PRIVATE_DEPS})
if(NH_CORE_INCLUDE_DIRS)
    target_include_directories(nodehammer_lib SYSTEM PUBLIC ${NH_CORE_INCLUDE_DIRS})
endif()

# In-tree code gates on NH_WITH_TGEO / NH_WITH_LUA / NH_WITH_VIEWER, so the
# archive propagates them.
target_compile_definitions(nodehammer_lib PUBLIC ${NH_CORE_DEFINES} ${NH_APP_DEFINES})

# NH_STATIC collapses NH_API to nothing (include/nodehammer/api.hpp). PUBLIC so
# every in-tree consumer — CLI, viewer, tests — inherits it and no target has to
# remember; without it they would see the dllimport spelling on Windows and fail
# to link against the archive.
target_compile_definitions(nodehammer_lib PUBLIC NH_STATIC)

# The generated FlatBuffers headers are PRIVATE to the core in nh_core_common,
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

    # NH_CORE_SOURCES only — no NH_APP_SOURCES. See that list for the line and
    # for why --no-undefined below is what verifies it.
    add_library(nodehammer_shared SHARED ${NH_CORE_SOURCES})
    nh_core_common(nodehammer_shared)

    # Every dependency PRIVATE, and wrapped in $<BUILD_INTERFACE:> so it is
    # dropped from the generated export set. That is what spares an external
    # consumer a find_dependency() for zstd/flatbuffers/manifold it has no way to
    # satisfy and no reason to know about (#41 §10) — the shared library absorbs
    # its static dependencies whole, so they are resolved before a consumer ever
    # sees it. NH_APP_DEPS is absent entirely: watcher belongs to sources this
    # target does not compile.
    foreach(dep IN LISTS NH_CORE_DEPS NH_CORE_PRIVATE_DEPS)
        target_link_libraries(nodehammer_shared PRIVATE "$<BUILD_INTERFACE:${dep}>")
    endforeach()
    if(NH_CORE_INCLUDE_DIRS)
        target_include_directories(nodehammer_shared SYSTEM PRIVATE ${NH_CORE_INCLUDE_DIRS})
    endif()

    # PRIVATE, unlike the archive. By §5 no public header contains an #if, so a
    # consumer has no use for NH_WITH_*, and pushing NH_WITH_LUA=1 into somebody
    # else's translation units is a collision waiting to happen. This is what
    # keeps INTERFACE_COMPILE_DEFINITIONS empty in the generated export, which is
    # in turn what makes the installed headers self-describing.
    target_compile_definitions(nodehammer_shared PRIVATE ${NH_CORE_DEFINES})

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
    # archive to hidden, and it is the flag that does the work we actually care
    # about: measured, it is the difference between two third-party symbols
    # escaping and none.
    #
    # --no-undefined then asserts the other half of §10's claim: that a shared
    # library resolves its private dependencies internally, so an external
    # consumer needs no find_dependency() for them.
    #
    # ELF-only, like cmake/PicProbe.cmake and for the same reason: ld64 has no
    # equivalent of either flag (only per-archive -hidden-l), so on macOS the
    # dependencies' symbols do stay visible in the dylib. ci/check_shared_exports.py
    # is correspondingly Linux-only.
    #
    # What deliberately is *not* here is a linker version script. These two flags
    # leave the std:: template instantiations that libstdc++ forces to default
    # visibility (`namespace std _GLIBCXX_VISIBILITY(default)`), and a script
    # restricting the table to `nodehammer::*` would remove them — at a price not
    # worth paying. libstdc++ exports those on purpose so they merge across
    # shared objects, and this library's own API passes std:: types across the
    # boundary anyway, so hiding its std:: internals buys no ABI independence it
    # does not already lack. It also costs: ld matches `extern "C++"` patterns
    # against the whole demangled name, and `vtable for nodehammer::X` does not
    # start with `nodehammer::`, so such a script silently drops the vtable and
    # typeinfo of every public class — an over-removal that no export-table check
    # can observe, because a symbol that should be there and is not leaves
    # nothing to inspect. See ci/check_shared_exports.py for what is asserted
    # instead.
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

# Every install rule below names a COMPONENT — Runtime for what an end user
# needs to run nodehammer, Development for what a consumer needs to build
# against it. `cmake --install` with no --component still installs everything
# ("during a full installation all components are installed"), so this changes
# nothing for the existing callers; what it adds is the ability to ask for one
# half, which is how a release artifact takes the CLI and its licences without
# the headers and the CMake package config.
#
# Tagging is all-or-nothing in practice: an untagged rule lands in a default
# component named Unspecified, so it would be silently omitted from *both*
# `--component Runtime` and `--component Development`. That is why the LICENSE,
# the wasm sidecars and cmake/ThirdPartyLicenses.cmake are annotated here too,
# and not only the library rules that motivated this. `list_install_components`
# reports what exists.
install(TARGETS ${NH_INSTALL_TARGETS}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
)

# Emscripten produces a sidecar .wasm next to each .js loader; CMake's
# install(TARGETS RUNTIME) only picks up the primary output, so install
# the blobs explicitly.
if(EMSCRIPTEN)
    foreach(_t ${NH_INSTALL_TARGETS})
        install(FILES "$<TARGET_FILE_DIR:${_t}>/${_t}.wasm"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
        )
    endforeach()
endif()

install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}"
    COMPONENT Runtime
)

# ── Library install: the shared library, the public headers, a package config ──
# Gated on NODEHAMMER_BUILD_SHARED, which is what makes the split load-bearing
# rather than aspirational: the only headers that can reach a consumer are the
# ones under include/nodehammer, because that is the only directory named here.
# There is no manifest of public headers to keep in sync with reality and no
# annotation an internal header could carry by mistake — an internal header is
# unreachable because it lives somewhere else on disk (#41 §1).
if(NODEHAMMER_BUILD_SHARED)
    # The component split follows the artefacts' actual audiences, and for a
    # versioned shared library that split runs *through* install(TARGETS LIBRARY)
    # rather than around it. NAMELINK_COMPONENT is what expresses it: the real
    # file and its SONAME symlink (libnodehammer.so.0.0.0, libnodehammer.so.0)
    # are Runtime, because anything that already links nodehammer needs them at
    # load time; the bare libnodehammer.so devlink is Development, because only a
    # linker resolving -lnodehammer ever looks at it. Windows has no namelink —
    # its equivalent division is the .dll (Runtime) against the .lib import
    # library (Development), which the RUNTIME and ARCHIVE lines already state.
    install(TARGETS nodehammer_shared
        EXPORT nodehammer-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                COMPONENT Runtime
                NAMELINK_COMPONENT Development
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}   # Windows import library
                COMPONENT Development
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # Windows DLL
                COMPONENT Runtime
    )

    # FILES_MATCHING "*.hpp" also excludes version.hpp.in, which would otherwise
    # install an uninstantiated template next to the header generated from it.
    install(DIRECTORY include/nodehammer
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT Development
        FILES_MATCHING PATTERN "*.hpp"
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nodehammer"
        COMPONENT Development
    )

    install(EXPORT nodehammer-targets
        FILE nodehammer-targets.cmake
        NAMESPACE nodehammer::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer"
        COMPONENT Development
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
        COMPONENT Development
    )
endif()

# Reads the manifest produced by conanfile.py's generate(), assembles
# THIRD-PARTY-NOTICES.txt at configure time, and installs it + the staged
# per-dep folders. Silent no-op when no manifest is present (non-Conan path).
include(cmake/ThirdPartyLicenses.cmake)

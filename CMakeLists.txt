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

# Record where external shared dependencies were found, so an installed
# libnodehammer says how to load itself rather than relying on the environment.
# Only ever has an effect when a backend is enabled: with no backends every
# dependency is a static archive absorbed into the library, and there is nothing
# to point at. The case it exists for is ROOT/DD4hep, which stay DT_NEEDED
# references.
#
# What keeps that from becoming an assumption about the build machine is the tag
# it lands in. DT_RUNPATH is searched *after* LD_LIBRARY_PATH, so a sourced LCG
# view still wins and the recorded path is only a fallback; DT_RPATH is searched
# before, and would override a deliberately set environment. Modern binutils
# already default to RUNPATH — verified on gcc:14, where the flag changes
# nothing — so --enable-new-dtags pins that default rather than establishing it,
# which is worth one line because the failure it prevents would be silent.
# Mach-O has its own @rpath mechanism and no equivalent flag; Windows has no
# concept; and Emscripten counts as UNIX to CMake but its wasm-ld rejects the
# flag outright, so it needs excluding explicitly.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    add_link_options("LINKER:--enable-new-dtags")
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
option(NODEHAMMER_BUILD_PYTHON  "Build the nanobind Python extension module (the wheel payload)" OFF)

# DD4hep's importer *is* a TGeo importer with extra passes: it calls
# `traverseTGeoManager`, and the importer registry registers `TGeoImporter`
# whenever NH_WITH_TGEO is defined — which enabling DD4hep already does, since
# that define is on the union of the two options. But the TU defining both of
# those symbols was gated on NODEHAMMER_WITH_TGEO alone, so a DD4hep-only build
# compiled two call sites for code nobody compiled and failed to link on both.
#
# Forcing the option on is the fix rather than widening that one source gate: it
# keeps the define, the registry entry, the `.root` importer and the traversal
# moving together, so there is one answer to "does this build have TGeo" instead
# of two that can drift. Latent until now only because no configuration builds
# DD4hep without TGeo — CI's LCG job sets both.
if(NODEHAMMER_WITH_DD4HEP AND NOT NODEHAMMER_WITH_TGEO)
    message(STATUS "nodehammer: NODEHAMMER_WITH_DD4HEP implies NODEHAMMER_WITH_TGEO; enabling it")
    set(NODEHAMMER_WITH_TGEO ON CACHE BOOL "Enable TGeo/ROOT importer" FORCE)
endif()

# The extension links the shared library and reaches it only through
# include/nodehammer, which is what makes "the bindings use only the public API"
# a link-time fact instead of a review rule (docs/python-bindings-plan.md). There
# is nothing else for it to link, so this is a forced implication rather than an
# error, the same shape as the DD4hep rule above.
if(NODEHAMMER_BUILD_PYTHON AND NOT NODEHAMMER_BUILD_SHARED)
    message(STATUS "nodehammer: NODEHAMMER_BUILD_PYTHON implies NODEHAMMER_BUILD_SHARED; enabling it")
    set(NODEHAMMER_BUILD_SHARED ON CACHE BOOL
        "Also build and install the shared library + public headers + CMake package config" FORCE)
endif()

if(NODEHAMMER_BUILD_PYTHON AND EMSCRIPTEN)
    message(FATAL_ERROR "NODEHAMMER_BUILD_PYTHON is native-only: there is no CPython to extend under Emscripten.")
endif()

include(cmake/Dependencies.cmake)
include(cmake/CompilerOptions.cmake)

# Included early: the core-library helper below bakes CMAKE_INSTALL_INCLUDEDIR
# into the shared target's INSTALL_INTERFACE include path.
include(GNUInstallDirs)

# ── The web runtime's contract ───────────────────────────────────────────────
# The wasm viewer is delivered as a directory, not a program: a shell, a worker
# script, three bundles and the stamp below. `nodehammer viewer --web` locates
# one through a ladder of candidate directories (a flag, an environment
# variable, this install path, later a download cache), which means it can be
# handed a directory that nobody validated — an old checkout's build tree is
# exactly the mistake a person makes while debugging, and no wheel metadata or
# version pin can see it. So the runtime says what it is and the library checks
# before it serves anything.
#
# What is compared is the schema id, not the version. What actually has to
# agree between the two halves is the .nhproj + nh_manifest.json shape and the
# compute worker's postMessage protocol — the last of which has already broken
# once, silently, under Closure (dcc4a06, and scripts/check_wasm_module_
# linkage.py exists because of it). An integer is at once stricter than a
# version, because it catches same-version skew from a hand-pointed directory,
# and looser, because it does not cry wolf on a patch release where nothing
# about the contract moved. The version rides along in the stamp only so the
# refusal can name it.
#
# BUMP THIS when the manifest/archive shape or the worker protocol changes.
# Monotonic; never reused.
set(NH_WEB_RUNTIME_SCHEMA 1)
set(NH_WEB_RUNTIME_STAMP "nh_runtime.json")
set(NH_WEB_RUNTIME_DIR "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/web")

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
# + scene_build, and — since the CLI became callable rather than only runnable —
# the subcommand dispatchers too.
#
# That reads like a reversal of the rule this list used to state ("deliberately
# free of CLI11 and the cmd_*.cpp dispatchers, so the bench harness, the test
# binary and the browser entry point can link against it without pulling
# subcommand registration into their closure"), and it is not: a static archive
# is linked per object, so a target that names no registrar still links none of
# them. The bench harness and the compute module are unchanged in size. What
# does change is the *shared* library, which has no such granularity and now
# carries the CLI — which is the entire point, since that is the copy the Python
# extension links, and `nodehammer::cli::run` is what it is there to reach.
#
# Accumulated into variables rather than written onto a target, because a
# packaging build describes the core once and builds it twice: the static
# archive every in-tree target links, and the installed shared library. Anything
# appended to *this* list reaches both — that is where the "describe once"
# guarantee lives. NH_APP_SOURCES below is the deliberate exception.
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
    src/ir/fb/render/exporter.cpp
    src/ir/json/semantic/exporter.cpp
    src/ir/fb/semantic/exporter.cpp
    src/ir/semantic/exporter_registry.cpp
    src/detail/markup.cpp
    # Host-side, despite the directory: locating and vetting the wasm runtime is
    # something the *serving* process does. src/web/'s other two TUs are the
    # Emscripten entry points and are not core.
    src/web/runtime_locator.cpp
    src/web/executable_path.cpp
    src/scene_build.cpp
    src/export_resolve.cpp
    src/version.cpp
    # The public API bridge: the only translation units that see both a public
    # handle and its internals (src/api/handles.hpp is where the seam lives).
    # Core, not app — these *are* what the installed library publishes.
    src/api/diagnostics.cpp
    src/api/semantic_scene.cpp
    src/api/config.cpp
    src/api/render_scene.cpp
    src/api/build.cpp
    # The in-memory bag (key→bytes store) behind ProjectFs. Under src/viewer/ for
    # organisational reasons, but no sokol/GUI deps and part of the project
    # substrate rather than the app — see NH_APP_SOURCES for where that line is.
    src/viewer/bag_project_fs.cpp
    # Archive-internal project manifest (nodehammer.toml [project]) parser — pure
    # toml++, no GUI deps, so it lives in the lib and the unit tests link it.
    src/viewer/project_manifest.cpp
    # The command line. `run.cpp` holds the entry point the public
    # include/nodehammer/cli.hpp declares; the rest are its subcommands.
    #
    # Core rather than app because `cli::run` is published: the whole reason
    # these moved out of a static archive is that an archive cannot be linked
    # into the Python extension — it would put a second copy of the core in the
    # process, with duplicate globals and two type_infos for
    # `nodehammer::Error`, so a `catch` across that boundary would silently miss.
    #
    # `cmd_viewer_native.cpp` is deliberately absent: it constructs a
    # `viewer::App`, and --no-undefined on the shared library turns that into a
    # link error naming the sokol symbol rather than a rule someone must
    # remember. It is compiled into the executable instead, and reaches the
    # parser through the `Registrar` seam in src/cli/run_internal.hpp.
    src/cli/run.cpp
    src/cli/pager.cpp
    src/cli/cmd_convert.cpp
    src/cli/cmd_inspect.cpp
    src/cli/cmd_validate_config.cpp
    src/cli/cmd_config_flatten.cpp
    src/cli/cmd_dump_semantic.cpp
    src/cli/cmd_dump_render.cpp
    src/cli/cmd_config_lua.cpp
)

# ── Application sources ───────────────────────────────────────────────────────
# Static archive only, never the installed shared library: the library is the
# headless pipeline core, and a consumer wants neither a camera nor a UI state
# machine — nor watcher's Apple CoreServices/CoreFoundation link for a file
# watcher it never calls.
#
# Note the line is *not* `src/viewer/*`. That directory also holds the
# project/VFS/archive stack, which is the .nhproj capability and stays core.
# These four (plus watched_filesystem_project_fs below) only ever reach *into*
# the core, never out of it — which is what makes removing them safe, and what
# --no-undefined checks: misfile a pipeline source here and the shared link
# fails naming the symbol.
set(NH_APP_SOURCES
    src/viewer/app_state.cpp
    src/viewer/camera.cpp
    # Adaptive render-scale state machine. GPU-free, so the tests link it
    # without the viewer.
    src/viewer/dynamic_render_scale.cpp
    # resolve→parse→import driver. App-side for now; a candidate to promote if
    # step 6 exposes it.
    src/viewer/build_session.cpp
)

# Appended to below, alongside their sources.
set(NH_APP_DEPS "")
set(NH_APP_DEFINES "")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nodehammer/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp
    @ONLY
)

# PUBLIC on the archive, because an internal header names the dependency where an
# out-of-core target sees it. Each entry cites the header that justifies it;
# remove one and the build fails. (Scope on the shared library is a different
# question — there everything is PRIVATE behind $<BUILD_INTERFACE:>.)
set(NH_CORE_DEPS
    # PUBLIC on the static archive rather than private, because an *internal*
    # header names it where an out-of-core target sees it: src/cli/cli_common.hpp
    # includes <CLI/CLI.hpp>, and src/cli/cmd_viewer_native.cpp is compiled
    # outside the core. A PRIVATE dependency propagates as $<LINK_ONLY:>, which
    # carries the link and drops the include directory — so the viewer
    # executable would fail to find the header. Header-only, so this adds no
    # link dependency to the shared library and nothing to its package config.
    CLI11::CLI11
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
# tinygltf being PRIVATE is what makes nlohmann's PUBLIC entry above meaningful:
# tinygltf lists nlohmann_json in its own INTERFACE_LINK_LIBRARIES, so while it
# was PUBLIC every in-tree target got nlohmann regardless of what this file said
# (#41's "nlohmann can finally go PRIVATE" row — the answer turned out to be that
# nlohmann is correctly PUBLIC and tinygltf was the over-declared one).
set(NH_CORE_PRIVATE_DEPS
    tomlplusplus::tomlplusplus
    # tests/export/test_gltf_exporter.cpp includes <tiny_gltf.h> directly and
    # declares this itself — see tests/CMakeLists.txt.
    TinyGLTF::TinyGLTF
)

set(NH_CORE_DEFINES "")

# The web runtime's schema id, compiled in so `web::compiledSchema()` and the
# `nh_runtime.json` the install rules stamp come from one `set()` and cannot
# drift. See the contract block above GNUInstallDirs.
list(APPEND NH_CORE_DEFINES NH_WEB_RUNTIME_SCHEMA=${NH_WEB_RUNTIME_SCHEMA})

# Extra include directories for dependencies that ship variables instead of
# imported targets — Geant4 is the only one today. Everything else propagates
# its includes through the link.
set(NH_CORE_INCLUDE_DIRS "")

# ── Core library: optional pieces ─────────────────────────────────────────────
# Everything below appends to the three lists above, so it reaches both the
# static and the shared variant. find_package() calls stay here rather than
# inside the helper: they are configuration, not target description, and must
# run exactly once.

# Lua config front-end: a source in the core rather than a library of its own,
# because a separate target could not put the engine *inside* the shared library
# for `Config::read`'s `.lua` branch (docs/config-scripting-lua.md, #41 §6).
#
# Unconditional, Emscripten included. The gate here used to say "no lua under
# Emscripten", which read like a constraint and was a build accident: the
# interpreter compiles and runs there once Conan builds it as C++, and the one
# thing the gate reliably produced was a public API that varied by platform —
# `Config::formats()` answering "toml" in a browser and "toml, lua" on a desktop
# for the same source tree. See conanfile.py's `configure` for why C++ and not C.
#
# The wasm bundles do not pay for this. Nothing in the viewer or the compute
# worker calls `Config::read` on a path, so static-lib dead-strip leaves the
# interpreter out of both — the cost lands on the call site that wants it, which
# is the property that makes shipping it everywhere reasonable.
#
# lua_config.cpp silences -Wconversion/-Wsign-conversion via a scoped #pragma
# (sol2's proxy conversions are not clean under them), kept in the TU so it
# cannot leak to the sources it shares a target with.
list(APPEND NH_CORE_SOURCES src/lua/lua_config.cpp)
# lua_config.hpp exposes only config::ConfigResult, so the interpreter and its
# binding stay inside the core's implementation.
list(APPEND NH_CORE_PRIVATE_DEPS lua::lua sol2::sol2)

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
#
# The web viewer's server and staging are native-only for a plainer reason:
# they exist to *serve* the wasm runtime to a browser, and a wasm module is the
# thing being served. Sockets are the giveaway -- there are none in the sandbox.
# The locator above is not gated, because "where would the runtime be" is a
# question the wasm build can be asked and answer "nowhere" to.
if(NOT EMSCRIPTEN)
    list(APPEND NH_CORE_SOURCES
        src/viewer/filesystem_project_fs.cpp
        src/viewer/native_bag_project_fs.cpp
        src/web/static_server.cpp
        src/web/stage.cpp
        src/web/open_browser.cpp
        # Packing mounts a FilesystemProjectFs, so it is gated here rather than
        # beside the ZipWorkingSet it builds -- that half *is* cross-platform.
        src/project/pack.cpp
        src/cli/cmd_project.cpp
        # `viewer --web` itself. Excluded here rather than #ifdef'd inside:
        # every line of it calls something in the three files above, so a wasm
        # build of this TU would be a shell around a refusal. `run.cpp` skips
        # the registration to match, and a wasm module simply has no `viewer`
        # subcommand — which is right, since it *is* the viewer.
        src/cli/cmd_viewer_web.cpp
    )
    # Private: httplib.h is included by static_server.cpp and by nothing else,
    # least of all a header. Header-only, so the shared library gains no link
    # dependency and its package config gains no requirement.
    list(APPEND NH_CORE_PRIVATE_DEPS httplib::httplib)
endif()

# WatchedFilesystemProjectFs (strategy doc step 4) decorates FilesystemProjectFs
# and uses wtr.watcher to bump generation() on disk changes. Viewer-gated like
# platform_folders, native-only like the FS backend it wraps.
#
# App-side, and the source that makes the split pay: watcher is the one
# dependency whose recipe propagates the Apple CoreServices/CoreFoundation
# frameworks, and an installed libnodehammer never calls it. Promote it if a
# consumer wants a watching ProjectFs — what it decorates is already core.
if(NOT EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    list(APPEND NH_APP_SOURCES src/viewer/watched_filesystem_project_fs.cpp)
    find_package(watcher CONFIG REQUIRED)
    # PUBLIC on the archive: watched_filesystem_project_fs.hpp and viewer/app.hpp
    # both name wtr types, and the viewer libs include them.
    list(APPEND NH_APP_DEPS watcher::watcher)
endif()

# ZipWorkingSet (strategy doc step 5) + ArchiveProjectFs (step 6) + archive
# export (step 7): the .nhproj capability. Cross-platform — only the POSIX save
# internals are native-only, guarded inside the .cpp.
#
# Unconditional on purpose. Gating this on the viewer made the *installed* library
# vary with an application feature: a viewer-off build produced a libnodehammer
# that could not open a project. miniz is the cheapest dependency to make
# unconditional (plain C, no OS deps, builds for Emscripten), and binaries that
# never touch an archive are unaffected — static-lib dead-strip drops the objects,
# the same mechanism that keeps BagProjectFs out of the wasm viewer.
list(APPEND NH_CORE_SOURCES
    src/viewer/zip_working_set.cpp
    src/viewer/archive_project_fs.cpp
    src/viewer/archive_export.cpp
)
find_package(miniz CONFIG REQUIRED)
# No header mentions miniz; ZipWorkingSet keeps it behind its .cpp.
list(APPEND NH_CORE_PRIVATE_DEPS miniz::miniz)

# NH_WITH_VIEWER is an app define: nothing in the core gates on it (the only use
# in the whole tree is src/cli/main.cpp, reached via the archive's PUBLIC
# propagation), so the shared library has no business carrying it.
if(NODEHAMMER_WITH_VIEWER)
    list(APPEND NH_APP_DEFINES NH_WITH_VIEWER=1)
endif()

# ── Core library targets ──────────────────────────────────────────────────────
# The settings that do not depend on which variant is being built. No parameter
# beyond the target, deliberately: the "describe once" guarantee lives in the
# NH_CORE_* lists, not here, so anything variant-specific is written out at the
# two call sites instead of hidden behind a discriminator.
function(nh_core_common target)
    # `include/` holds the public headers; internal ones live next to their
    # source under `src/`. The BUILD_INTERFACE genex keeps src/ on the include
    # path in-tree while dropping it from the exported interface, so nothing
    # internal *can* reach an install (#41 §1).
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    # Everything here is built as C++23. What each variant *asks of its
    # consumers* differs, so that is stated at the two call sites rather than
    # here.
    target_compile_features(${target} PRIVATE cxx_std_23)

    add_dependencies(${target} flatbuffers_generate)
    target_include_directories(${target} SYSTEM PRIVATE ${NH_FBS_GENERATED_DIR})

    nh_set_compiler_options(${target})
    nh_set_visibility(${target})
endfunction()

# ── Shared core objects (non-Windows) ─────────────────────────────────────────
# A shared build compiles the core twice, once per variant. On Windows that is
# unavoidable — __declspec(dllexport) is codegen-affecting, so a DLL's objects
# are not valid inside a static library. Elsewhere NH_API only ever *raises*
# visibility (include/nodehammer/visibility.hpp), so one object serves both and an
# OBJECT library compiles the core once.
#
# The trade: shared objects cannot carry NH_STATIC, so the archive's *public*
# entities keep default visibility instead of collapsing to nothing. That matters
# only to something linking the archive into a shared object, which would then
# re-export the public API — #41 §10 assumes the Python bindings do exactly that,
# though linking libnodehammer instead would avoid it entirely. Either way
# internals stay hidden (they were never NH_API) and third-party symbols remain
# --exclude-libs's job.
if(NODEHAMMER_BUILD_SHARED AND NOT WIN32)
    add_library(nodehammer_core_objs OBJECT ${NH_CORE_SOURCES})
    nh_core_common(nodehammer_core_objs)

    # Needed to link into the .so; harmless in the archive, which
    # cmake/PicProbe.cmake requires to be PIC anyway.
    set_property(TARGET nodehammer_core_objs PROPERTY POSITION_INDEPENDENT_CODE ON)

    # PRIVATE throughout: this target exists to be compiled, not depended on.
    # Both consumers re-declare dependencies at the scope they want to propagate.
    target_link_libraries(nodehammer_core_objs PRIVATE ${NH_CORE_DEPS} ${NH_CORE_PRIVATE_DEPS})
    target_compile_definitions(nodehammer_core_objs PRIVATE ${NH_CORE_DEFINES})
    if(NH_CORE_INCLUDE_DIRS)
        target_include_directories(nodehammer_core_objs SYSTEM PRIVATE ${NH_CORE_INCLUDE_DIRS})
    endif()

    set(NH_CORE_INPUT $<TARGET_OBJECTS:nodehammer_core_objs>)
else()
    set(NH_CORE_INPUT ${NH_CORE_SOURCES})
endif()

# ── Static archive ────────────────────────────────────────────────────────────
# What every in-tree target links — CLI, viewer, tests, benchmarks. Core + app
# sources. Never installed, so its dependencies are PUBLIC: in-tree targets reach
# toml++, glm and nlohmann through this link.
add_library(nodehammer_lib STATIC ${NH_CORE_INPUT} ${NH_APP_SOURCES})
nh_core_common(nodehammer_lib)

target_link_libraries(nodehammer_lib PUBLIC ${NH_CORE_DEPS} ${NH_APP_DEPS})
target_link_libraries(nodehammer_lib PRIVATE ${NH_CORE_PRIVATE_DEPS})
if(NH_CORE_INCLUDE_DIRS)
    target_include_directories(nodehammer_lib SYSTEM PUBLIC ${NH_CORE_INCLUDE_DIRS})
endif()

# In-tree code gates on NH_WITH_TGEO / NH_WITH_VIEWER, so the archive
# propagates them.
target_compile_definitions(nodehammer_lib PUBLIC ${NH_CORE_DEFINES} ${NH_APP_DEFINES})

# In-tree consumers — CLI, viewer, tests, benchmarks — are C++23 code and reach
# internal headers that use it. This is what keeps them at 23 rather than
# leaving them to inherit CMAKE_CXX_STANDARD by luck. The archive is never
# installed, so this never reaches a package config.
target_compile_features(nodehammer_lib INTERFACE cxx_std_23)

# NH_STATIC collapses NH_API to nothing (include/nodehammer/visibility.hpp). PUBLIC so
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

    # Core only — no NH_APP_SOURCES. NH_CORE_INPUT is either the sources or the
    # shared objects, per the OBJECT block above.
    add_library(nodehammer_shared SHARED ${NH_CORE_INPUT})
    nh_core_common(nodehammer_shared)

    # Plain PRIVATE is enough to keep the generated export empty, so a consumer
    # needs no find_dependency() (#41 §10). CMake propagates a static library's
    # PRIVATE dependencies as $<LINK_ONLY:dep> — the consumer still has to link
    # them — but a *shared* library resolves its own, so nothing lands in
    # INTERFACE_LINK_LIBRARIES at all. Verified both ways: the property reads
    # NOTFOUND here, and wrapping each entry in $<BUILD_INTERFACE:> produces a
    # byte-identical nodehammer-targets.cmake.
    #
    # NH_APP_DEPS is absent entirely — watcher belongs to sources this target
    # does not compile.
    target_link_libraries(nodehammer_shared PRIVATE ${NH_CORE_DEPS} ${NH_CORE_PRIVATE_DEPS})
    if(NH_CORE_INCLUDE_DIRS)
        target_include_directories(nodehammer_shared SYSTEM PRIVATE ${NH_CORE_INCLUDE_DIRS})
    endif()

    # PRIVATE, unlike the archive: no public header has an #if (#41 §5), so a
    # consumer has no use for NH_WITH_*, and pushing them into someone else's
    # translation units invites a collision. Keeps INTERFACE_COMPILE_DEFINITIONS
    # empty in the export.
    target_compile_definitions(nodehammer_shared PRIVATE ${NH_CORE_DEFINES})

    # The floor an external consumer actually needs, which is not what we build
    # with: the installed headers use std::string_view and inline constexpr and
    # nothing later, verified by compiling them standalone at each level (C++14
    # and below fail, 17 upwards pass). docs/public-api-sketch.md already names
    # C++17 as the floor for the amalgamated route and calls it "the binding
    # constraint". CMAKE_CXX_STANDARD does not survive an export, so the floor
    # has to be stated on the target to reach the package config.
    #
    # The constraint is on API vocabulary rather than on the build: a type in the
    # public interface has to exist at the floor, putting std::span (C++20) and
    # std::expected (C++23) out of reach until it moves. ci/shared_consumer
    # compiles at exactly the floor so that is enforced rather than remembered.
    # The floor an external consumer needs, which is not what we build with.
    # Today's installed headers need only C++17 — verified by compiling them
    # standalone at each level, where 14 and below fail — so 20 is deliberately
    # headroom rather than a description, chosen so step 6 can use std::span in
    # the API. It matches the "convert" tier in docs/public-api-sketch.md's
    # language-floor table. CMAKE_CXX_STANDARD does not survive an export, so
    # this has to be stated on the target to reach the package config.
    #
    # The constraint is on API vocabulary rather than on the build: a type in the
    # public interface has to exist at the floor, which still puts std::expected
    # (C++23) out of reach. ci/shared_consumer compiles at exactly the floor, so
    # that is enforced rather than remembered.
    target_compile_features(nodehammer_shared INTERFACE cxx_std_20)

    # NH_EXPORTS selects the dllexport spelling of NH_API while compiling the
    # library itself; PRIVATE, since a consumer must see dllimport instead.
    target_compile_definitions(nodehammer_shared PRIVATE NH_EXPORTS)

    # The exported name is `nodehammer`; the target cannot be, because the CLI
    # executable already owns that target name.
    set_target_properties(nodehammer_shared PROPERTIES
        OUTPUT_NAME nodehammer
        EXPORT_NAME nodehammer
    )

    # Version the library only when it is going to be installed as a system
    # library. A wheel is a zip, and pip extracts zips with `zipfile`, which does
    # not restore symlinks — so the libnodehammer.so -> .so.0 -> .so.0.1.3 chain
    # cannot survive the trip. Flattening it at install time is not an option
    # either: what the extension records in DT_NEEDED / LC_LOAD_DYLIB is the
    # *soname*, i.e. the middle link, which is precisely the file that is a
    # symlink in a normal install.
    #
    # Dropping VERSION/SOVERSION makes the soname `libnodehammer.so` /
    # `@rpath/libnodehammer.dylib`, the linker emits exactly one real file, and
    # the extension's $ORIGIN/@loader_path rpath finds it with no symlink
    # anywhere. The cost is that the wheel's copy carries no ABI tag, which is
    # acceptable because nothing outside the wheel is meant to link it — the
    # wheel ships both halves together by construction.
    if(NOT NODEHAMMER_BUILD_PYTHON)
        set_target_properties(nodehammer_shared PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}
        )
    else()
        # CMAKE_INSTALL_RPATH_USE_LINK_PATH (top of this file) exists for the
        # ROOT/DD4hep case, where a DT_NEEDED reference must be findable on the
        # machine that built it. A wheel is the opposite: every dependency is a
        # static archive absorbed into this .so, there is nothing left to point
        # at, and an absolute build-machine path in RUNPATH is the one thing
        # auditwheel and delocate cannot repair.
        set_target_properties(nodehammer_shared PROPERTIES
            INSTALL_RPATH_USE_LINK_PATH OFF
            INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>"
            MACOSX_RPATH ON
        )
    endif()

    # Hidden visibility only governs objects *we* compile; the static
    # dependencies absorbed into this .so were not, so their symbols would land
    # in the dynamic table wholesale. --exclude-libs,ALL forces archive symbols
    # hidden and is the flag doing the work here. --no-undefined then asserts the
    # rest of §10's claim: the library resolves its private dependencies
    # internally, so a consumer needs no find_dependency().
    #
    # ELF-only, like cmake/PicProbe.cmake: ld64 has no equivalent (only
    # per-archive -hidden-l), so on macOS the dependencies' symbols do stay
    # visible and ci/check_shared_exports.py is correspondingly Linux-only.
    #
    # Deliberately *no* linker version script. It would only remove the std::
    # instantiations libstdc++ forces to default visibility — which it does on
    # purpose, so they merge across shared objects, and this API passes std::
    # types across the boundary anyway. It would also break public classes: ld
    # matches `extern "C++"` patterns against the whole demangled name, and
    # `vtable for nodehammer::X` does not start with `nodehammer::`, so a
    # `nodehammer::*` script silently drops every public vtable and typeinfo —
    # an over-removal no export-table check can see.
    if(UNIX AND NOT APPLE)
        target_link_options(nodehammer_shared PRIVATE
            "LINKER:--exclude-libs,ALL"
            "LINKER:--no-undefined"
        )
    endif()
endif()

# ── Optional viewer (sokol + Dear ImGui) ──────────────────────────────────────
# When the viewer is on, we build a per-backend sokol STATIC lib + a viewer
# executable per backend. Native picks the right backend for the host
# (Metal on Apple, GLCORE on Linux/Windows). Emscripten builds BOTH GLES3
# and WGPU executables from a single configure so the wasm shell can pick
# at runtime via navigator.gpu — see web/index.html.
if(NODEHAMMER_WITH_VIEWER)
    # Defines nodehammer_shaders custom target + NH_SHADER_INCLUDE_DIR.
    add_subdirectory(shaders)
    # Defines nh_add_viewer_lib(name sokol_lib).
    add_subdirectory(src/viewer)

    # Compose one viewer executable. The native build keeps the CLI
    # dispatcher (`nodehammer viewer …` alongside the other subcommands);
    # the wasm build skips CLI11 entirely and uses src/web/viewer_main.cpp,
    # which exposes a single `nh_viewer_start(opts_json)` C export driven
    # by web/index.html.
    function(nh_add_viewer_exe name viewer_lib)
        if(EMSCRIPTEN)
            add_executable(${name} src/web/viewer_main.cpp)
            target_link_libraries(${name} PRIVATE ${viewer_lib})
            nh_apply_emscripten_viewer_options(${name})
        else()
            add_executable(${name} src/cli/main.cpp src/cli/cmd_viewer_native.cpp)
            target_link_libraries(${name} PRIVATE nodehammer_lib ${viewer_lib})
            nh_apply_emscripten_exe_options(${name})
        endif()
        nh_set_compiler_options(${name})
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
        nh_apply_emscripten_compute_options(nodehammer-compute)
        nh_set_compiler_options(nodehammer-compute)
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
        nh_set_compiler_options(imgui-kitchen-sink)
    endif()
else()
    # Headless build: the entry point with no viewer subcommand handed to it.
    add_executable(nodehammer src/cli/main.cpp)
    target_link_libraries(nodehammer PRIVATE nodehammer_lib)
    nh_set_compiler_options(nodehammer)
    nh_apply_emscripten_exe_options(nodehammer)
endif()

# ── gcc 15: a false positive on copying a vector<SelectionRule> ───────────────
# Copying a `SelectionRule` copies a `PredicateExpr`, which is a `std::variant`
# holding `shared_ptr`s for the compound predicates. At -O3, gcc 15 inlines the
# variant's copy constructor together with the destructor that only ever runs if
# that construction *throws*, then reports the control-block pointer on that
# unreachable path as maybe-uninitialized. NODEHAMMER_WERROR makes it fatal, and
# `LCG 109` is the only configuration that compiles with this compiler.
#
# Declared as a list rather than as a `#pragma` in the offending function,
# because which function is "offending" is an inlining decision and moves: the
# first version of this suppression sat in src/api/build.cpp, and deleting an
# unrelated dead `catch` in that file moved the report to scene_build.cpp. These
# are the translation units that construct a `SelectionEngine`, which is where
# the copy happens; if a sixth appears, it goes here.
#
# Scoped per source so a genuine maybe-uninitialized anywhere else still fails.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(
        src/api/build.cpp
        src/scene_build.cpp
        src/selection/selector.cpp
        src/cli/cmd_convert.cpp
        src/cli/cmd_dump_semantic.cpp
        PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized"
    )
endif()

# ── Tests ──────────────────────────────────────────────────────────────────────
if(NODEHAMMER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Python extension ───────────────────────────────────────────────────────────
if(NODEHAMMER_BUILD_PYTHON)
    add_subdirectory(src/python)
endif()

# Included last: the probe links every library target that can end up inside a
# shared object, so all of them have to exist first.
include(cmake/PicProbe.cmake)

# ── Install ────────────────────────────────────────────────────────────────────
# (GNUInstallDirs is included near the top — the core-library helper needs
# CMAKE_INSTALL_INCLUDEDIR for the shared target's INSTALL_INTERFACE.)

# Every install rule names a COMPONENT: Runtime to run nodehammer, Development
# to build against it, WebRuntime for the servable wasm payload. A plain
# `cmake --install` still installs everything, so existing callers are
# unaffected; what this adds is `--component Runtime` for a release artifact
# that wants the CLI and its licences but not the headers.
#
# Tag every rule, not just the new ones — an untagged rule lands in a default
# component named Unspecified and would be silently missing from *both* halves.
if(EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    # This configuration does not install a *program*. What it installs is a
    # directory a web server can point at: the shell, the worker script, the
    # per-backend bundles and a stamp saying what they are. Six blobs under
    # bin/ were never that — `scripts/build_pages_site.sh` only worked because
    # it read the shell out of the source tree it happened to be running in, so
    # the artifact alone could not be served by anything that did not also have
    # a checkout.
    #
    # share/nodehammer/web is also the rung the asset ladder looks for relative
    # to the executable (docs/cli-and-web-viewer-plan.md Part 1), which is why
    # it is a datadir path and not an opaque one: a native install tree and a
    # wasm install tree merge into one directory that `nodehammer viewer --web`
    # can resolve without being told where anything is.
    install(TARGETS nodehammer-gles3 nodehammer-wgpu nodehammer-compute
        RUNTIME DESTINATION "${NH_WEB_RUNTIME_DIR}"
                COMPONENT WebRuntime
    )

    # Emscripten produces a sidecar .wasm next to each .js loader; CMake's
    # install(TARGETS RUNTIME) only picks up the primary output, so install
    # the blobs explicitly.
    foreach(_t nodehammer-gles3 nodehammer-wgpu nodehammer-compute)
        install(FILES "$<TARGET_FILE_DIR:${_t}>/${_t}.wasm"
            DESTINATION "${NH_WEB_RUNTIME_DIR}"
            COMPONENT WebRuntime
        )
    endforeach()

    # The two files the browser loads that no compiler produced. They are as
    # much a part of the runtime as the bundles — index.html is what picks a
    # backend at all, and a bundle without its worker script tessellates on the
    # main thread or not at all.
    install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/compute_worker.js"
        DESTINATION "${NH_WEB_RUNTIME_DIR}"
        COMPONENT WebRuntime
    )

    # The stamp, written beside the bundles in the build tree so that a build
    # directory and an install tree are the same shape — `just wasm-serve` and
    # `just pages-site` stage straight out of the former, and `viewer --web
    # --web-assets build/emscripten/Release` is the first thing any developer
    # reaches for.
    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:nodehammer-gles3>/${NH_WEB_RUNTIME_STAMP}"
        CONTENT "{\n  \"schema\": ${NH_WEB_RUNTIME_SCHEMA},\n  \"version\": \"${NODEHAMMER_VERSION}\"\n}\n"
    )
    install(FILES "$<TARGET_FILE_DIR:nodehammer-gles3>/${NH_WEB_RUNTIME_STAMP}"
        DESTINATION "${NH_WEB_RUNTIME_DIR}"
        COMPONENT WebRuntime
    )

    # "The same shape" has to include the two files no compiler produces, or the
    # claim above is false in exactly the way that bites: a build tree carrying a
    # stamp and bundles but no shell passes every check a stamp can make and then
    # serves 404 on `/`.
    #
    # Symlinks, not copies, and that is the whole point: an edit to web/index.html
    # is live in a served build tree with no rebuild, which is what `just
    # wasm-serve`'s link mode has always given. COPY_ON_ERROR covers Windows,
    # where an unprivileged symlink fails and a stale copy is better than an
    # incomplete directory.
    foreach(_nh_shell_file
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/web/compute_worker.js")
        get_filename_component(_nh_shell_name "${_nh_shell_file}" NAME)
        file(CREATE_LINK "${_nh_shell_file}"
             "${CMAKE_CURRENT_BINARY_DIR}/${_nh_shell_name}"
             COPY_ON_ERROR SYMBOLIC)
    endforeach()
else()
    # Native: the single nodehammer executable. Emscripten with no viewer: the
    # headless nodehammer bundle (just the .js + .wasm).
    install(TARGETS nodehammer
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                COMPONENT Runtime
    )

    if(EMSCRIPTEN)
        install(FILES "$<TARGET_FILE_DIR:nodehammer>/nodehammer.wasm"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
        )
    endif()
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
    # NAMELINK_COMPONENT splits one target across both components: the real file
    # and its SONAME symlink are Runtime (anything already linked needs them at
    # load time), while the bare libnodehammer.so devlink is Development (only a
    # linker resolving -lnodehammer reads it). Windows has no namelink; its
    # equivalent is .dll against .lib, stated by RUNTIME and ARCHIVE below.
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

    # The wheel payload: a second rule on the same target, into the Python
    # package directory. A third component beside Runtime and Development, for
    # the same reason those two exist — `cmake --install --component Python` has
    # to produce exactly what belongs in the wheel and nothing else: no CLI
    # executable, no headers, no package config. NAMELINK_SKIP is belt and
    # braces; there is no namelink to install when VERSION/SOVERSION are unset.
    if(NODEHAMMER_BUILD_PYTHON)
        install(TARGETS nodehammer_shared
            LIBRARY DESTINATION "${NH_PYTHON_PKG_DIR}"
                    COMPONENT Python
                    NAMELINK_SKIP
            RUNTIME DESTINATION "${NH_PYTHON_PKG_DIR}"   # Windows DLL
                    COMPONENT Python
        )
    endif()

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

    # Baked into the package config so a consumer's mismatched C runtime fails at
    # configure time rather than as a segmentation fault on the first call that
    # returns a container (see cmake/nodehammer-config.cmake.in).
    #
    # Evaluated by CMake rather than parsed by us. `CMAKE_MSVC_RUNTIME_LIBRARY`
    # is normally a generator expression, and its shape varies by who set it —
    # CMake's own default is "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" while
    # Conan writes "$<$<CONFIG:Release>:MultiThreadedDLL>". A hand-rolled
    # substitution for one of those spellings reported the other as a mismatch
    # and blocked a legitimate build, which is worse than the failure it exists
    # to catch. `file(GENERATE)` resolves whatever is there, exactly.
    #
    # The full runtime name, not a debug flag: /MT and /MD are different heaps
    # even when neither is a debug runtime, so a static-CRT library and a
    # dynamic-CRT consumer must not pass.
    if(MSVC)
        set(_nh_runtime_stamp "${CMAKE_CURRENT_BINARY_DIR}/nodehammer-msvc-runtime.txt")
        file(GENERATE OUTPUT "${_nh_runtime_stamp}"
             CONTENT "$<TARGET_PROPERTY:nodehammer_shared,MSVC_RUNTIME_LIBRARY>")
        install(FILES "${_nh_runtime_stamp}"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer"
            COMPONENT Development
        )
    endif()

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

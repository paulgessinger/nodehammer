# Assemble the single-header distributions, and prove one of them compiles.
#
# The artifact is for an experiment that wants nodehammer's conversion in its
# own process and cannot add a dependency to do it: no library, no link line,
# nothing to install. See docs/event-display-design.md §7 for what that buys and
# scripts/amalgamate.py for how the assembly works.
#
# CMake's job here is *resolution*, not text: every include directory below
# comes from a target the normal build already resolved, so the amalgamation is
# built from the same dependency versions as the library it mirrors and there is
# no second place where a version is chosen. The Python script only rearranges
# what it is handed.
#
# Off by default. It costs a Python run and, with the smoke test, a compile of a
# two-megabyte header; neither belongs in an ordinary build.

if(NOT NODEHAMMER_BUILD_AMALGAMATED)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(NH_AMALGAM_DIR ${CMAKE_CURRENT_BINARY_DIR}/amalgamation)
set(NH_AMALGAM_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate.py)

# ── Where the vendored headers live ──────────────────────────────────────────
#
# Resolved from the dependency graph the normal build already produced, never
# written down here. Getting this wrong is not a build failure but a silent one:
# an unresolvable include stays `#include <glm/glm.hpp>` in the output, so the
# header would look assembled while still requiring the dependency it promised
# the consumer would not need. Hence the check at the end.
#
# The package variables rather than the targets' INTERFACE_INCLUDE_DIRECTORIES,
# which sounds like the more principled source and is not: Conan writes those as
# config-specific generator expressions — `$<$<CONFIG:Release>:/path>` — which do
# not resolve at configure time, and for flatbuffers sets no such property at
# all, putting the include path only in the variable. The variables are plain
# paths from every generator that sets them.
#
# `<pkg>_INCLUDE_DIRS` is set by find_package for both Conan's config files and
# a plain system install; the target property is the fallback for a FetchContent
# build, where it is a real path rather than a genex.
set(NH_AMALGAM_VENDOR
    glm_INCLUDE_DIRS               glm::glm
    nlohmann_json_INCLUDE_DIRS     nlohmann_json::nlohmann_json
    unordered_dense_INCLUDE_DIRS   unordered_dense::unordered_dense
    flatbuffers_INCLUDE_DIRS       flatbuffers::flatbuffers
)

set(NH_AMALGAM_INLINE_DIRS "")
list(LENGTH NH_AMALGAM_VENDOR nh_vendor_len)
math(EXPR nh_vendor_last "${nh_vendor_len} / 2 - 1")
foreach(i RANGE ${nh_vendor_last})
    math(EXPR vi "${i} * 2")
    math(EXPR ti "${vi} + 1")
    list(GET NH_AMALGAM_VENDOR ${vi} var)
    list(GET NH_AMALGAM_VENDOR ${ti} tgt)

    set(dirs "")
    if(DEFINED ${var})
        set(dirs "${${var}}")
    elseif(TARGET ${tgt})
        get_target_property(prop ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
        if(prop)
            set(dirs "${prop}")
        endif()
    endif()

    set(found "")
    foreach(d IN LISTS dirs)
        # A genex here means the value came from a target and names a path this
        # step cannot learn; skip it rather than emit `$<...>` as a directory.
        if(NOT d MATCHES "\\$<" AND IS_DIRECTORY "${d}")
            list(APPEND found "${d}")
        endif()
    endforeach()

    if(NOT found)
        message(FATAL_ERROR
            "NODEHAMMER_BUILD_AMALGAMATED cannot locate headers for ${tgt}. "
            "Tried ${var}=[${${var}}] and its INTERFACE_INCLUDE_DIRECTORIES. "
            "The amalgamation vendors this dependency, so its headers have to "
            "be readable at configure time.")
    endif()
    list(APPEND NH_AMALGAM_INLINE_DIRS ${found})
endforeach()

# Ours, plus the flatc output. The generated schema headers are inlined like any
# other source: they include nothing but the flatbuffers runtime, which is
# vendored right alongside.
list(APPEND NH_AMALGAM_INLINE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NH_FBS_GENERATED_DIR}
)
list(REMOVE_DUPLICATES NH_AMALGAM_INLINE_DIRS)

set(NH_AMALGAM_INLINE_FLAGS "")
foreach(d IN LISTS NH_AMALGAM_INLINE_DIRS)
    list(APPEND NH_AMALGAM_INLINE_FLAGS --inline-dir ${d})
endforeach()

# ── One variant per manifest ─────────────────────────────────────────────────
#
# `nh_amalgamate(<stem>)` turns amalgamation/<stem>.json into
# ${NH_AMALGAM_DIR}/nodehammer_<stem>.h and hangs it off a target.
#
# DEPENDS lists the manifest and the script but not the sources it will read:
# which files those are is the manifest's answer, discovered by walking includes
# at build time, and restating it here would be a second copy free to disagree.
# The cost of that is a stale header when only a source changed, which the CI
# job regenerates from scratch anyway.
function(nh_amalgamate stem)
    set(manifest ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/${stem}.json)
    set(output ${NH_AMALGAM_DIR}/nodehammer_${stem}.h)

    add_custom_command(
        OUTPUT ${output}
        COMMAND ${Python3_EXECUTABLE} ${NH_AMALGAM_SCRIPT}
                --manifest ${manifest}
                --output ${output}
                ${NH_AMALGAM_INLINE_FLAGS}
                --version ${PROJECT_VERSION}
        DEPENDS ${NH_AMALGAM_SCRIPT} ${manifest} flatbuffers_generate
        COMMENT "Amalgamating nodehammer_${stem}.h"
        VERBATIM
    )
    add_custom_target(nodehammer_amalgam_${stem} ALL DEPENDS ${output})
    set(NH_AMALGAM_${stem}_HEADER ${output} PARENT_SCOPE)
endfunction()

# One header, always generated. Generating it needs no backend: the importers'
# `#include <TGeoManager.h>` does not resolve inside any --inline-dir, so it is
# left as a real include for whoever compiles the result, exactly like <vector>.
# Only *compiling* with the backends on needs ROOT and DD4hep.
nh_amalgamate(connect)

# Whether this tree can compile the configuration that carries the importers.
# The other configuration -- NH_WITH_TGEO=0, NH_WITH_DD4HEP=0 -- compiles
# anywhere, which is what the smoke test below uses.
set(NH_AMALGAM_HAVE_CONNECT OFF)
if(NODEHAMMER_WITH_TGEO AND NODEHAMMER_WITH_DD4HEP)
    set(NH_AMALGAM_HAVE_CONNECT ON)
endif()

# ── The smoke test ───────────────────────────────────────────────────────────
#
# Generating a header proves nothing; compiling one proves the concatenation
# produced a valid translation unit, and linking a second object against it
# proves the interface half holds only declarations. Two translation units
# deliberately: a single-TU test cannot see a definition that escaped the
# NH_IMPLEMENTATION guard, because there is nothing for it to collide with.
add_executable(nodehammer_amalgam_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/smoke.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/smoke_second_tu.cpp
)
add_dependencies(nodehammer_amalgam_smoke nodehammer_amalgam_connect)

# The backend-free configuration. The defines the header emits for itself are
# `#ifndef`-guarded precisely so a consumer can answer differently, and this is
# that consumer: the same shipped header, with the importers compiled out, so
# it needs neither ROOT nor DD4hep and builds on every platform in the matrix.
target_compile_definitions(nodehammer_amalgam_smoke PRIVATE NH_WITH_TGEO=0 NH_WITH_DD4HEP=0)

# The generated header's directory, and nothing else. Not linking
# nodehammer_lib, and not reaching src/ or include/ -- if any of it were needed
# the amalgamation would be incomplete, and the point is to find that out here.
target_include_directories(nodehammer_amalgam_smoke PRIVATE ${NH_AMALGAM_DIR})
target_compile_features(nodehammer_amalgam_smoke PRIVATE cxx_std_23)

if(NODEHAMMER_BUILD_TESTS)
    add_test(NAME amalgamation_smoke COMMAND nodehammer_amalgam_smoke)
endif()

# ── The connector, where the backends exist ──────────────────────────────────
#
# The variant that is actually shipped, and the only test that compiles the
# importers out of an amalgamation. It links ROOT and DD4hep, which is not a
# concession: the header leaves both as real `#include`s because the experiment
# already has them, so having them is what makes a complete-header claim
# testable at all.
#
# One translation unit here rather than two. The declaration/definition split is
# the same header's and is already covered by nodehammer_amalgam_smoke on every
# platform; what is unproven in the connector is whether the absorbed importers
# compile, which one TU answers.
if(NH_AMALGAM_HAVE_CONNECT)
    add_executable(nodehammer_amalgam_connect_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/smoke_connect.cpp
    )
    add_dependencies(nodehammer_amalgam_connect_smoke nodehammer_amalgam_connect)
    target_include_directories(nodehammer_amalgam_connect_smoke PRIVATE ${NH_AMALGAM_DIR})
    target_compile_features(nodehammer_amalgam_connect_smoke PRIVATE cxx_std_23)

    # The environment's own backends, and nothing of ours: no nodehammer_lib,
    # no src/, no include/. Anything else it turns out to need is a hole in the
    # amalgamation, which is the point of building it this way.
    target_link_libraries(nodehammer_amalgam_connect_smoke PRIVATE ROOT::Geom DD4hep::DDCore)

    if(NODEHAMMER_BUILD_TESTS)
        add_test(NAME amalgamation_connect_smoke COMMAND nodehammer_amalgam_connect_smoke)
    endif()
endif()

# ── Golden equivalence: the same input, the same bytes out ───────────────────
#
# The smoke tests prove the generated header compiles, links and runs. They do
# not prove it computes the same answer as the library it was cut from, and
# nothing did until this: a header that silently absorbed a source wrongly, or
# lost an `#if`, would pass every check above while producing a different scene.
# docs/event-display-design.md §10.2 asks for exactly this.
#
# Two programs, one geometry, one shared body — they differ only in how they
# reach `nodehammer::`. One links the amalgamated header and nothing else; the
# other links nodehammer_lib. A driver script runs both and compares the files,
# because the claim is about the pair and ctest runs one command per test.
#
# Only where the connector is buildable, for the reason the connector smoke test
# gives: this compares the shipped variant, which needs ROOT and DD4hep.
if(NH_AMALGAM_HAVE_CONNECT AND NODEHAMMER_BUILD_TESTS)
    add_executable(nodehammer_amalgam_golden
        ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/golden_amalgam.cpp
    )
    add_dependencies(nodehammer_amalgam_golden nodehammer_amalgam_connect)
    target_include_directories(nodehammer_amalgam_golden PRIVATE
        ${NH_AMALGAM_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation
    )
    target_compile_features(nodehammer_amalgam_golden PRIVATE cxx_std_23)
    # The environment's backends only -- same rule as the connector smoke test.
    target_link_libraries(nodehammer_amalgam_golden PRIVATE ROOT::Geom DD4hep::DDCore)

    add_executable(nodehammer_modular_golden
        ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/golden_modular.cpp
    )
    target_include_directories(nodehammer_modular_golden PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation
    )
    target_compile_features(nodehammer_modular_golden PRIVATE cxx_std_23)
    target_link_libraries(nodehammer_modular_golden PRIVATE nodehammer_lib ROOT::Geom)

    add_test(NAME amalgamation_golden_equivalence
        COMMAND ${CMAKE_COMMAND}
            -DAMALGAM=$<TARGET_FILE:nodehammer_amalgam_golden>
            -DMODULAR=$<TARGET_FILE:nodehammer_modular_golden>
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/amalgamation/golden
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/amalgam_golden_test.cmake
    )
endif()

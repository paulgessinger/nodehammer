# The version of record is a git tag, read here with `git describe`. There is one
# exception, and it comes first.
#
# Under scikit-build-core the version is decided on the Python side — setuptools_scm,
# which speaks PEP 440 — and handed down as SKBUILD_PROJECT_VERSION. Prefer it
# there, for two reasons. `git describe`'s spelling is not PEP 440, so the two
# would disagree by construction; and a wheel built from an sdist has no .git to
# describe at all, which would fall through to the branch below and ship a
# libnodehammer whose nodehammer::version() reports 0.0.0 from inside a wheel
# whose metadata says otherwise.
#
# The numeric prefix is what project(VERSION) and SOVERSION need; the full string
# is what version.hpp reports, so a development wheel says
# "0.1.3.post1.dev398+g920e6c5" — the same string pip shows, and strictly more
# information than the "0.1.3-398-g920e6c5" it replaces. The prefix match has no
# anchor at its end on purpose: a pre-release version keeps its numeric prefix
# and everything after it rides in the full string.
if(DEFINED SKBUILD_PROJECT_VERSION
        AND SKBUILD_PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(NODEHAMMER_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(NODEHAMMER_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(NODEHAMMER_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(NODEHAMMER_VERSION "${SKBUILD_PROJECT_VERSION}")
    return()
endif()

execute_process(
    COMMAND git describe --tags --long --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _git_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_result
)

# The `(.*)` between the patch number and the commit distance is what makes a
# pre-release tag a version rather than a fallthrough. `v0.2.0rc1` is spelled the
# way setuptools_scm and PEP 440 want it, so the two halves of one release agree,
# and it does not match a triple-only pattern — which is how tagging v0.2.0rc1
# silently rewrote nodehammer::version(), the SOVERSION and the package config
# version to 0.0.0 for every native and wasm build off that tag, while wheels
# went on being right because they take the branch above.
#
# Greedy, and it backtracks to the last `-<digits>-g<hash>`, so both spellings
# work: "v0.2.0rc1-3-gabc1234" and "v0.2.0-rc1-3-gabc1234" both yield the triple
# 0.2.0 with the pre-release riding in the full string. The suffix is passed
# through, not validated — this file's job is to report the tag that exists, not
# to have an opinion about how it is spelled.
if(_git_result EQUAL 0 AND _git_describe MATCHES
        "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)-([0-9]+)-g([0-9a-f]+)$")
    # Named up front: six groups read as six groups, and CMAKE_MATCH_* is global
    # state that the next `if(... MATCHES ...)` anywhere would overwrite.
    set(NODEHAMMER_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(NODEHAMMER_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(NODEHAMMER_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(_nh_pre "${CMAKE_MATCH_4}")
    set(_nh_distance "${CMAKE_MATCH_5}")
    set(_nh_hash "${CMAKE_MATCH_6}")

    set(_nh_base
        "${NODEHAMMER_VERSION_MAJOR}.${NODEHAMMER_VERSION_MINOR}.${NODEHAMMER_VERSION_PATCH}${_nh_pre}")
    if(_nh_distance EQUAL 0)
        set(NODEHAMMER_VERSION "${_nh_base}")
    else()
        set(NODEHAMMER_VERSION "${_nh_base}-${_nh_distance}-g${_nh_hash}")
    endif()
else()
    # Loud, because it is silent failure that made this branch worth fixing: the
    # build carries on and every version it reports is a lie, in four places at
    # once, and nothing says so until someone reads one of them.
    if(NOT _git_result EQUAL 0)
        set(_nh_why "`git describe` failed — not a git checkout, or git is not installed")
    else()
        set(_nh_why "`git describe` returned \"${_git_describe}\", which names no vMAJOR.MINOR.PATCH tag")
    endif()
    message(WARNING
        "nodehammer: could not derive a version — ${_nh_why}.\n"
        "Falling back to 0.0.0, which is what nodehammer --version, "
        "nodehammer::version(), the shared library SOVERSION and "
        "nodehammer-config-version.cmake will all report — so "
        "find_package(nodehammer <any version>) against this build will fail.\n"
        "A tagless or shallow clone is the usual cause; `git fetch --tags` fixes it.")

    set(NODEHAMMER_VERSION_MAJOR 0)
    set(NODEHAMMER_VERSION_MINOR 0)
    set(NODEHAMMER_VERSION_PATCH 0)
    set(NODEHAMMER_VERSION "0.0.0")
endif()

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
# information than the "0.1.3-398-g920e6c5" it replaces.
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

if(_git_result EQUAL 0 AND _git_describe MATCHES
        "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)$")
    set(NODEHAMMER_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(NODEHAMMER_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(NODEHAMMER_VERSION_PATCH "${CMAKE_MATCH_3}")
    if(CMAKE_MATCH_4 EQUAL 0)
        set(NODEHAMMER_VERSION
            "${NODEHAMMER_VERSION_MAJOR}.${NODEHAMMER_VERSION_MINOR}.${NODEHAMMER_VERSION_PATCH}")
    else()
        set(NODEHAMMER_VERSION
            "${NODEHAMMER_VERSION_MAJOR}.${NODEHAMMER_VERSION_MINOR}.${NODEHAMMER_VERSION_PATCH}-${CMAKE_MATCH_4}-g${CMAKE_MATCH_5}")
    endif()
else()
    set(NODEHAMMER_VERSION_MAJOR 0)
    set(NODEHAMMER_VERSION_MINOR 0)
    set(NODEHAMMER_VERSION_PATCH 0)
    set(NODEHAMMER_VERSION "0.0.0")
endif()

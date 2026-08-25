# Run the skills generator over a fixture tree and check what it wrote.
#
# The build already proves the generator works for the skills that happen to be
# in `skills/` today. What it cannot prove is that the generator works for the
# *next* one -- and that is where it broke: identifiers were spelled
# `kFiles_${name}`, so a skill directory called `nodehammer-config` emitted
# `kFiles_nodehammer-config`, which a C++ compiler reads as a subtraction. One
# skill with a plain name built fine and hid it completely.
#
# So the fixture carries the awkward cases on purpose: a name that is not a
# valid identifier, and a file nested below the skill's own directory. Run in
# script mode by ctest; no compiler needed, because the property that broke is
# visible in the text.
#
# Inputs: NH_GENERATOR, NH_FIXTURE_DIR, NH_WORK_DIR.

cmake_minimum_required(VERSION 3.25)

foreach(var NH_GENERATOR NH_FIXTURE_DIR NH_WORK_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "generate_skills_data_test.cmake: ${var} is required")
    endif()
endforeach()

set(generated "${NH_WORK_DIR}/skills_data_fixture.cpp")
file(REMOVE "${generated}")
file(MAKE_DIRECTORY "${NH_WORK_DIR}")

file(GLOB_RECURSE fixture_files "${NH_FIXTURE_DIR}/*")
if(fixture_files STREQUAL "")
    message(FATAL_ERROR "the fixture at ${NH_FIXTURE_DIR} is empty")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DNH_SKILLS_DIR=${NH_FIXTURE_DIR}
        "-DNH_SKILLS_FILES=${fixture_files}"
        -DNH_SKILLS_OUT=${generated}
        -P ${NH_GENERATOR}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the generator failed (${rc}):\n${out}\n${err}")
endif()
if(NOT EXISTS "${generated}")
    message(FATAL_ERROR "the generator wrote nothing to ${generated}")
endif()

file(READ "${generated}" source)

# ── Every identifier it declares must be one ─────────────────────────────────
#
# The regression itself. Matching the declarations and checking each against the
# C++ rule catches any future scheme that derives an identifier from a name,
# not merely the dash that happened to be found first.
string(REGEX MATCHALL "constexpr [A-Za-z_]+ ([A-Za-z0-9_]*[^A-Za-z0-9_ ][A-Za-z0-9_-]*)\\[\\]"
       ill_formed "${source}")
if(NOT ill_formed STREQUAL "")
    message(FATAL_ERROR
        "the generator emitted an ill-formed identifier -- a skill name reached "
        "C++ source instead of staying in a string literal:\n${ill_formed}")
endif()

# ── Both skills, with their real names, and the nested path ──────────────────
foreach(needle "\"plain\"" "\"with-dash\"" "\"references/nested.md\"")
    string(FIND "${source}" "${needle}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "the generated source does not mention ${needle}")
    endif()
endforeach()

# Sorted, so `skills list` does not depend on what order the filesystem answered
# in: "plain" must be written before "with-dash".
string(FIND "${source}" "{\"plain\"" plain_at)
string(FIND "${source}" "{\"with-dash\"" dash_at)
if(plain_at EQUAL -1 OR dash_at EQUAL -1 OR NOT plain_at LESS dash_at)
    message(FATAL_ERROR "the skills table is not sorted by name")
endif()

# ── Rerunning is not a rewrite ───────────────────────────────────────────────
#
# The generated file feeds a translation unit that everything links, so an
# identical regeneration that still touched the content would recompile the
# world on every build.
file(TIMESTAMP "${generated}" before "%Y%m%d%H%M%S")
file(READ "${generated}" first_pass)
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DNH_SKILLS_DIR=${NH_FIXTURE_DIR}
        "-DNH_SKILLS_FILES=${fixture_files}"
        -DNH_SKILLS_OUT=${generated}
        -P ${NH_GENERATOR}
    RESULT_VARIABLE rc
)
file(READ "${generated}" second_pass)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the generator failed on a second run (${rc})")
endif()
if(NOT first_pass STREQUAL second_pass)
    message(FATAL_ERROR "the generator is not deterministic: two runs over one "
                        "fixture produced different source")
endif()

message(STATUS "generator fixture ok: ${NH_FIXTURE_DIR}")

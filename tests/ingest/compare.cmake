# Drive the golden equivalence check: run both programs, compare the bytes.
#
# A script rather than two tests because the claim is about the *pair* — either
# file alone says nothing, and ctest runs one command per test.
#
# Invoked as:
#   cmake -DINGEST=<exe> -DMODULAR=<exe> -DWORKDIR=<dir> -P compare.cmake

foreach(var INGEST MODULAR WORKDIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} not set")
    endif()
endforeach()

set(ingest_out "${WORKDIR}/golden_ingest.nhb")
set(modular_out "${WORKDIR}/golden_modular.nhb")

file(MAKE_DIRECTORY "${WORKDIR}")
file(REMOVE "${ingest_out}" "${modular_out}")

execute_process(COMMAND "${INGEST}" "${ingest_out}" RESULT_VARIABLE rc_a)
if(NOT rc_a EQUAL 0)
    message(FATAL_ERROR "the ingestion program failed (${rc_a})")
endif()

execute_process(COMMAND "${MODULAR}" "${modular_out}" RESULT_VARIABLE rc_m)
if(NOT rc_m EQUAL 0)
    message(FATAL_ERROR "the modular program failed (${rc_m})")
endif()

file(SIZE "${ingest_out}" size_a)
file(SIZE "${modular_out}" size_m)

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files "${ingest_out}" "${modular_out}"
    RESULT_VARIABLE differ
)

if(NOT differ EQUAL 0)
    message(FATAL_ERROR "ingestion and full library outputs differ (${size_a} vs ${size_m} bytes):\n${ingest_out}\n${modular_out}")
endif()

message(STATUS "golden equivalence: ${size_a} bytes, identical")

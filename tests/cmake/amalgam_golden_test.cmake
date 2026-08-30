# Drive the golden equivalence check: run both programs, compare the bytes.
#
# A script rather than two tests because the claim is about the *pair* — either
# file alone says nothing, and ctest runs one command per test.
#
# Invoked as:
#   cmake -DAMALGAM=<exe> -DMODULAR=<exe> -DWORKDIR=<dir> -P amalgam_golden_test.cmake

foreach(var AMALGAM MODULAR WORKDIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} not set")
    endif()
endforeach()

set(amalgam_out "${WORKDIR}/golden_amalgam.nhb")
set(modular_out "${WORKDIR}/golden_modular.nhb")

file(MAKE_DIRECTORY "${WORKDIR}")
file(REMOVE "${amalgam_out}" "${modular_out}")

execute_process(COMMAND "${AMALGAM}" "${amalgam_out}" RESULT_VARIABLE rc_a)
if(NOT rc_a EQUAL 0)
    message(FATAL_ERROR "the amalgamated program failed (${rc_a})")
endif()

execute_process(COMMAND "${MODULAR}" "${modular_out}" RESULT_VARIABLE rc_m)
if(NOT rc_m EQUAL 0)
    message(FATAL_ERROR "the modular program failed (${rc_m})")
endif()

file(SIZE "${amalgam_out}" size_a)
file(SIZE "${modular_out}" size_m)

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files "${amalgam_out}" "${modular_out}"
    RESULT_VARIABLE differ
)

if(NOT differ EQUAL 0)
    # Say which way it went. A size difference means the two builds disagree
    # about the *scene* -- a shape absorbed differently, a node dropped. Equal
    # sizes with different content is the floating-point case: the amalgamation
    # is one translation unit, so the compiler inlines across boundaries the
    # modular build keeps, and a transform product can land a bit or two apart.
    if(NOT size_a EQUAL size_m)
        message(FATAL_ERROR
            "the amalgamated and modular builds produced different scenes: "
            "${size_a} bytes vs ${size_m}.\n"
            "  ${amalgam_out}\n  ${modular_out}")
    endif()
    message(FATAL_ERROR
        "the amalgamated and modular builds produced the same scene with "
        "different bytes (${size_a} each).\n"
        "Most likely a floating-point difference from single-TU inlining. If "
        "that is accepted, this test needs to compare the decoded scene within "
        "a tolerance rather than the file.\n"
        "  ${amalgam_out}\n  ${modular_out}")
endif()

message(STATUS "golden equivalence: ${size_a} bytes, identical")

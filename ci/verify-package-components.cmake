# Configure independent consumers so previously imported targets cannot hide a
# component-selection mistake. Run after either minimal or full SDK installation.
foreach(required PREFIX BUILD_DIR HAS_SHARED)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()
set(cases default ingest shared optional unknown)
if(HAS_SHARED)
    list(APPEND cases repeat)
endif()
foreach(case IN LISTS cases)
    execute_process(COMMAND ${CMAKE_COMMAND}
        -S ${CMAKE_CURRENT_LIST_DIR}/package_components
        -B ${BUILD_DIR}/${case} -G Ninja
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=${PREFIX}
        -DCASE=${case} -DHAS_SHARED=${HAS_SHARED}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(case STREQUAL "unknown" OR (case STREQUAL "shared" AND NOT HAS_SHARED))
        if(case STREQUAL "unknown")
            set(missing Unknown)
        else()
            set(missing Shared)
        endif()
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "Component '${missing}' is not installed")
            message(FATAL_ERROR "Expected a clear missing-component failure (${case}):\n${output}${error}")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "Component test ${case} failed:\n${output}${error}")
    endif()
    message(STATUS "Package component test ${case}: passed")
endforeach()

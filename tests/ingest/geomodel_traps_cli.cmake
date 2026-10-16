file(REMOVE "${OUTPUT}")
# Import and semantic output must neither tessellate nor report approximation warnings.
execute_process(COMMAND "${CLI}" convert -i "${INPUT}" -o "${OUTPUT}" --strict
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT EXISTS "${OUTPUT}" OR err MATCHES "NH1303")
    message(FATAL_ERROR "Parametric trap import failed: ${status} ${err}")
endif()
file(SIZE "${OUTPUT}" size)
if(size GREATER 10000)
    message(FATAL_ERROR "Single-trap NHB should store parameters, not a mesh: ${size}")
endif()
file(WRITE "${OUTPUT}.toml" "[defaults.tessellation]\ngeneric_trap_tolerance = 0.02\n")
execute_process(COMMAND "${CLI}" convert -i "${OUTPUT}" -o "${OUTPUT}.glb" -c "${OUTPUT}.toml"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT EXISTS "${OUTPUT}.glb")
    message(FATAL_ERROR "Deferred trap rendering failed: ${status} ${err}")
endif()
file(WRITE "${OUTPUT}.toml" "[defaults.tessellation]\ngeneric_trap_tolerance = 0\n")
execute_process(COMMAND "${CLI}" convert -i "${OUTPUT}" -o "${OUTPUT}.invalid.glb" -c "${OUTPUT}.toml"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(status EQUAL 0)
    message(FATAL_ERROR "Invalid render tolerance must reject conversion")
endif()

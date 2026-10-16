file(REMOVE "${OUTPUT}")
execute_process(COMMAND "${CLI}" convert -i "${INPUT}" -o "${OUTPUT}"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(status EQUAL 0 OR EXISTS "${OUTPUT}" OR NOT err MATCHES "NH1301")
    message(FATAL_ERROR "Opaque shapes must reject default conversion: ${status} ${err}")
endif()
execute_process(COMMAND "${CLI}" convert -i "${INPUT}" -o "${OUTPUT}" --geomodel-allow-opaque-shapes
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT EXISTS "${OUTPUT}" OR NOT err MATCHES "NH1301")
    message(FATAL_ERROR "Explicit opaque export failed: ${status} ${err}")
endif()
execute_process(COMMAND "${CLI}" inspect summary -i "${INPUT}" --geomodel-allow-opaque-shapes --output-format json
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(JSON kind ERROR_VARIABLE json_error GET "${out}" kind)
if(NOT status EQUAL 0 OR NOT kind STREQUAL "summary")
    message(FATAL_ERROR "GeoModel reader polluted JSON stdout: ${json_error} ${out} ${err}")
endif()
file(REMOVE "${OUTPUT}")
execute_process(COMMAND "${CLI}" convert -i "${INPUT}" -o "${OUTPUT}" --geomodel-allow-opaque-shapes --strict
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(status EQUAL 0 OR EXISTS "${OUTPUT}")
    message(FATAL_ERROR "Strict policy must reject opaque warning")
endif()

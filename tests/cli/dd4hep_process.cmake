file(MAKE_DIRECTORY "${WORKDIR}")

function(import_geometry driver input output)
    execute_process(COMMAND "${driver}" -i "${input}" -o "${output}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT out STREQUAL "" OR NOT EXISTS "${output}")
        message(FATAL_ERROR "DD4hep import failed (${code}): stdout=${out}\nstderr=${err}")
    endif()
    execute_process(COMMAND "${NODEHAMMER}" inspect --output-format json summary -i "${output}"
        RESULT_VARIABLE code OUTPUT_VARIABLE summary ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "NHB readback failed: ${err}")
    endif()
    string(JSON nodes GET "${summary}" nodes)
    if(NOT nodes EQUAL 2)
        message(FATAL_ERROR "Expected world + box, got ${summary}")
    endif()
endfunction()

# Exercise the installed entry point as well as the identical main linked with
# a test-only DD4hep factory (no external k4geo installation needed in CI).
import_geometry("${DRIVER}" "${FIXTURES}/simple_box.xml" "${WORKDIR}/simple.nhb")
foreach(extension nhb nhb.zst)
    import_geometry("${PROBE}" "${FIXTURES}/global_detector.xml" "${WORKDIR}/global.${extension}")
endforeach()
execute_process(COMMAND "${NODEHAMMER}" inspect --output-format json tags
    -i "${WORKDIR}/global.nhb.zst"
    RESULT_VARIABLE code OUTPUT_VARIABLE tags ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "Tag readback failed: ${err}")
endif()
string(JSON sensitive GET "${tags}" keys sensitive 0)
if(NOT sensitive STREQUAL "true")
    message(FATAL_ERROR "Lost DD4hep sensitivity metadata: ${tags}")
endif()

# Like ALLEGRO's missing-constant path, this plugin terminates its process.
# It must not kill the consumer, publish a file, or contaminate the next import.
file(READ "${FIXTURES}/global_detector.xml" xml)
string(REPLACE "name=\"plugin_exit_code\" value=\"0\""
    "name=\"plugin_exit_code\" value=\"19\"" xml "${xml}")
file(WRITE "${WORKDIR}/exit.xml" "${xml}")
file(REMOVE "${WORKDIR}/exit.nhb")
execute_process(COMMAND "${PROBE}" -i "${WORKDIR}/exit.xml" -o "${WORKDIR}/exit.nhb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 19 OR NOT out STREQUAL "" OR EXISTS "${WORKDIR}/exit.nhb")
    message(FATAL_ERROR "Plugin exit was not isolated (${code}): ${out}\n${err}")
endif()
import_geometry("${PROBE}" "${FIXTURES}/global_detector.xml" "${WORKDIR}/after-exit.nhb")

# Validate output names before loading any plugin or creating an output.
file(REMOVE "${WORKDIR}/invalid.glb")
execute_process(COMMAND "${PROBE}" -i "${WORKDIR}/exit.xml" -o "${WORKDIR}/invalid.glb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 1 OR NOT err MATCHES "NH0900" OR EXISTS "${WORKDIR}/invalid.glb")
    message(FATAL_ERROR "Invalid output was not rejected before loading: ${code} ${err}")
endif()
file(WRITE "${WORKDIR}/broken.xml" "<lccdd><broken")
file(REMOVE "${WORKDIR}/broken.nhb")
execute_process(COMMAND "${DRIVER}" -i "${WORKDIR}/broken.xml" -o "${WORKDIR}/broken.nhb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 1 OR NOT out STREQUAL "" OR NOT err MATCHES "NH0300"
   OR EXISTS "${WORKDIR}/broken.nhb")
    message(FATAL_ERROR "Malformed compact was not reported: ${code} ${out}\n${err}")
endif()

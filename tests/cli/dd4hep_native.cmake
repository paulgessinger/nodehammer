file(MAKE_DIRECTORY "${WORKDIR}/input with spaces")
file(COPY_FILE "${FIXTURES}/global_detector.xml" "${WORKDIR}/input with spaces/global.xml")

# Both flag positions must work. Plugin chatter must not pollute JSON stdout.
foreach(placement before after)
    if(placement STREQUAL "before")
        set(args --dd4hep-global inspect --output-format json summary)
    else()
        set(args inspect --output-format json summary --dd4hep-global)
    endif()
    execute_process(COMMAND "${PROBE}" ${args} -i "${WORKDIR}/input with spaces/global.xml"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR err MATCHES "NH0302")
        message(FATAL_ERROR "Global import failed (${code}): ${out}\n${err}")
    endif()
    string(JSON nodes GET "${out}" nodes)
    if(NOT nodes EQUAL 2)
        message(FATAL_ERROR "Expected world + box, got ${out}")
    endif()
endforeach()

foreach(extension nhb nhb.zst glb)
    execute_process(COMMAND "${PROBE}" convert --dd4hep-global
        -i "${FIXTURES}/global_detector.xml" -o "${WORKDIR}/global.${extension}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT out STREQUAL "" OR err MATCHES "NH0302"
       OR NOT EXISTS "${WORKDIR}/global.${extension}")
        message(FATAL_ERROR "Global conversion failed (${code}): ${out}\n${err}")
    endif()
endforeach()
execute_process(COMMAND "${NODEHAMMER}" inspect --output-format json tags
    -i "${WORKDIR}/global.nhb.zst"
    RESULT_VARIABLE code OUTPUT_VARIABLE tags ERROR_VARIABLE err)
string(JSON sensitive GET "${tags}" keys sensitive 0)
if(NOT code EQUAL 0 OR NOT sensitive STREQUAL "true")
    message(FATAL_ERROR "Lost sensitivity metadata: ${tags}\n${err}")
endif()

# Direct XML selection must use DD4hep names, without an intermediate NHB.
file(WRITE "${WORKDIR}/select.toml"
    "[[selection_rules]]\ndrop_if = 'true'\n[[selection_rules]]\nkeep_if = 'path ~= \"/world\" || path ~= \"/world/GlobalProbe\"'\n")
execute_process(COMMAND "${PROBE}" convert --dd4hep-global
    -i "${FIXTURES}/global_detector.xml" -c "${WORKDIR}/select.toml" -o "${WORKDIR}/selected.nhb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "Direct XML selection failed: ${code} ${err}")
endif()
execute_process(COMMAND "${NODEHAMMER}" inspect --output-format json summary
    -i "${WORKDIR}/selected.nhb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(JSON nodes GET "${out}" nodes)
if(NOT code EQUAL 0 OR NOT nodes EQUAL 2)
    message(FATAL_ERROR "Selection lost renamed DD4hep nodes: ${code} ${out} ${err}")
endif()

# Private detector + plugin exit: preserve exit status and explain the opt-in,
# including in quiet mode. No output should be published.
file(REMOVE "${WORKDIR}/private.nhb")
execute_process(COMMAND "${PROBE}" -q convert -i "${FIXTURES}/global_detector.xml"
    -o "${WORKDIR}/private.nhb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 19 OR NOT out STREQUAL "" OR NOT err MATCHES "NH0302"
   OR NOT err MATCHES "Retry with --dd4hep-global" OR EXISTS "${WORKDIR}/private.nhb")
    message(FATAL_ERROR "Missing private-import exit warning (${code}): ${out}\n${err}")
endif()

file(READ "${FIXTURES}/global_detector.xml" xml)
foreach(kind exit abort)
    if(kind STREQUAL "exit")
        set(plugin_code 19)
    else()
        set(plugin_code -1)
    endif()
    string(REPLACE "name=\"plugin_exit_code\" value=\"0\""
        "name=\"plugin_exit_code\" value=\"${plugin_code}\"" failing_xml "${xml}")
    file(WRITE "${WORKDIR}/${kind}.xml" "${failing_xml}")
    file(REMOVE "${WORKDIR}/${kind}.nhb")
    execute_process(COMMAND "${PROBE}" convert --dd4hep-global
        -i "${WORKDIR}/${kind}.xml" -o "${WORKDIR}/${kind}.nhb"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT out STREQUAL "" OR EXISTS "${WORKDIR}/${kind}.nhb")
        message(FATAL_ERROR "Failed import published output: ${out}\n${err}")
    endif()
    if(kind STREQUAL "exit")
        if(NOT code EQUAL 19 OR NOT err MATCHES "NH0302" OR err MATCHES "Retry with")
            message(FATAL_ERROR "Incorrect global-import exit warning: ${code} ${err}")
        endif()
    elseif(code EQUAL 0 OR err MATCHES "NH0302")
        message(FATAL_ERROR "abort bypasses atexit and must not claim an exit warning: ${code} ${err}")
    endif()
endforeach()

file(WRITE "${WORKDIR}/broken.xml" "<lccdd><broken")
foreach(flag "" --dd4hep-global)
    execute_process(COMMAND "${NODEHAMMER}" convert ${flag}
        -i "${WORKDIR}/broken.xml" -o "${WORKDIR}/broken.nhb"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 1 OR NOT err MATCHES "NH0300" OR err MATCHES "NH0302")
        message(FATAL_ERROR "Malformed XML should unwind normally: ${code} ${err}")
    endif()
    execute_process(COMMAND "${NODEHAMMER}" inspect ${flag} --output-format json summary
        -i "${FIXTURES}/simple_box.xml"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR err MATCHES "NH0302")
        message(FATAL_ERROR "Simple XML should succeed without exit warning: ${code} ${err}")
    endif()
    string(JSON nodes GET "${out}" nodes)
    if(NOT nodes EQUAL 2)
        message(FATAL_ERROR "Unexpected simple geometry: ${out}")
    endif()
endforeach()

foreach(mode repeat populated callable)
    execute_process(COMMAND "${HOST}" "${FIXTURES}/simple_box.xml" "${mode}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT out MATCHES "HOST_RETURNED" OR err MATCHES "NH0302")
        message(FATAL_ERROR "Global detector lifecycle test failed: ${code} ${out}\n${err}")
    endif()
endforeach()

# Backend-specific CLI options must not be ignored for other formats.
execute_process(COMMAND "${NODEHAMMER}" convert --dd4hep-global
    -i synthetic --input-format synthetic -o "${WORKDIR}/wrong-format.nhb"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 1 OR NOT err MATCHES "NH0105")
    message(FATAL_ERROR "Wrong-format options were ignored: ${code} ${out} ${err}")
endif()

file(WRITE "${WORKDIR}/scene.toml" "")
execute_process(COMMAND "${PROBE}" project pack --dd4hep-global
    --config "${WORKDIR}/scene.toml" --input "${FIXTURES}/global_detector.xml"
    -o "${WORKDIR}/global.nhproj"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT EXISTS "${WORKDIR}/global.nhproj")
    message(FATAL_ERROR "Project pack lost importer options: ${code} ${out} ${err}")
endif()

# Read the manifest produced by conanfile.py's generate(), assemble a
# concatenated THIRD-PARTY-NOTICES.txt, and ship both the per-dep folders and
# the notices file alongside the binary. Silent no-op when no manifest is
# present (e.g. the non-Conan CMake path used inside LCG/CVMFS).

set(NH_TPL_DIR "${CMAKE_BINARY_DIR}/generators/third_party_licenses")
set(NH_TPL_MANIFEST "${NH_TPL_DIR}/manifest.json")
set(NH_TPL_NOTICES "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")

if(NOT EXISTS "${NH_TPL_MANIFEST}")
    return()
endif()

file(READ "${NH_TPL_MANIFEST}" _nh_tpl_json)

set(_nh_rule "------------------------------------------------------------------------")
set(_nh_sep  "========================================================================")
set(_nh_body
"Third-party notices for ${PROJECT_NAME} ${PROJECT_VERSION}
${_nh_sep}

The ${PROJECT_NAME} binary includes the following third-party software.
Each component is distributed under its own license, reproduced below.

")

string(JSON _nh_count LENGTH "${_nh_tpl_json}")
if(_nh_count GREATER 0)
    math(EXPR _nh_last "${_nh_count} - 1")
    foreach(idx RANGE 0 ${_nh_last})
        string(JSON _nh_entry GET "${_nh_tpl_json}" ${idx})
        string(JSON _nh_name    GET "${_nh_entry}" name)
        string(JSON _nh_version GET "${_nh_entry}" version)
        string(JSON _nh_license GET "${_nh_entry}" license)
        string(JSON _nh_dir     GET "${_nh_entry}" dir)
        string(JSON _nh_files   GET "${_nh_entry}" files)

        set(_nh_header "${_nh_name} ${_nh_version}")
        if(NOT _nh_license STREQUAL "")
            string(APPEND _nh_header " (${_nh_license})")
        endif()
        string(APPEND _nh_body
"${_nh_rule}
${_nh_header}
${_nh_rule}

")

        string(JSON _nh_file_count LENGTH "${_nh_files}")
        if(_nh_file_count GREATER 0)
            math(EXPR _nh_file_last "${_nh_file_count} - 1")
            foreach(fidx RANGE 0 ${_nh_file_last})
                string(JSON _nh_file GET "${_nh_files}" ${fidx})
                file(READ "${NH_TPL_DIR}/${_nh_dir}/${_nh_file}" _nh_text)
                string(STRIP "${_nh_text}" _nh_text)
                string(APPEND _nh_body "${_nh_text}\n\n")
            endforeach()
        endif()
    endforeach()
endif()

file(WRITE "${NH_TPL_NOTICES}" "${_nh_body}")

install(DIRECTORY "${NH_TPL_DIR}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/licenses"
    FILES_MATCHING
        PATTERN "*"
        PATTERN "manifest.json" EXCLUDE
)
install(FILES "${NH_TPL_NOTICES}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}"
)

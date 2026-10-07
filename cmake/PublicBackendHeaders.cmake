# Every adapter header exists. Disabled backends explain how to rebuild the
# library instead of failing with an unexplained missing header or link symbol.
foreach(_nh_backend tgeo dd4hep)
    string(TOUPPER "${_nh_backend}" _nh_backend_upper)
    set(_nh_header "${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/${_nh_backend}.hpp")
    if(NODEHAMMER_WITH_${_nh_backend_upper})
        set(_nh_adapter "${_nh_backend}.hpp")
    else()
        set(_nh_adapter "${_nh_backend}_disabled.hpp")
    endif()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/../src/api/public/${_nh_adapter}"
        "${_nh_header}" COPYONLY)
    if(NODEHAMMER_BUILD_SHARED)
        install(FILES "${_nh_header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nodehammer" COMPONENT Development)
    endif()
endforeach()

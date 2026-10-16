include_guard(GLOBAL)
include(FetchContent)

function(nh_geomodel_dependency)
    # Reuse the embedding application's targets, even when fetching is enabled.
    # Never introduce a second GeoModel into an experiment process.
    if(TARGET GeoModelCore::GeoModelKernel AND TARGET GeoModelIO::GeoModelRead)
        return()
    endif()
    if(NOT NODEHAMMER_FETCH_GEOMODEL)
        find_package(GeoModelCore 6.29.0 CONFIG REQUIRED)
        find_package(GeoModelIO 6.29.0 CONFIG REQUIRED)
        return()
    endif()
    if(TARGET GeoModelCore::GeoModelKernel OR TARGET GeoModelIO::GeoModelRead)
        message(FATAL_ERROR "Cannot fetch GeoModel alongside partial existing GeoModel targets; provide both Core and IO")
    endif()

    # Scope upstream options to this dependency, without changing the parent's
    # build/test settings. The pinned release builds Core and IO by default.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    foreach(feature ALL TOOLS EXAMPLES EXAMPLES_W_GEANT4 VISUALIZATION
            GEOMODELG4 FULLSIMLIGHT FULLSIMLIGHT_PROFILING FSL ATLASEXTENSIONS TESTING)
        set(GEOMODEL_BUILD_${feature} OFF)
    endforeach()
    foreach(dependency EIGEN3 XERCESC JSON)
        set(GEOMODEL_USE_BUILTIN_${dependency} OFF)
    endforeach()
    set(BUILD_TESTING OFF)
    # Keep the separate GeoModel install predictable on lib/lib64 platforms.
    set(CMAKE_INSTALL_LIBDIR lib)
    FetchContent_Declare(geomodel
        SYSTEM
        GIT_REPOSITORY https://gitlab.cern.ch/GeoModelDev/GeoModel.git
        GIT_TAG 961bfb7d3af556d875d1b416796fb07266b2bb9d # 6.29.0
        GIT_SUBMODULES ""
        # Large LFS detector examples are not needed for Core/IO.
        GIT_CONFIG filter.lfs.smudge= filter.lfs.process= filter.lfs.required=false
    )
    FetchContent_MakeAvailable(geomodel)
    # CMake 3.25-compatible equivalent of FetchContent's EXCLUDE_FROM_ALL.
    # GeoModel remains a separately installed runtime dependency, as on the
    # find_package path; its headers and exports do not enter the nodehammer SDK.
    set_property(DIRECTORY "${geomodel_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endfunction()

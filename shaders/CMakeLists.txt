# Shader build for the viewer (Stage 1: solid + imgui pipelines).
#
# bgfx_compile_shaders is provided by bgfxToolUtils.cmake (included from
# cmake/Dependencies.cmake when NODEHAMMER_WITH_VIEWER=ON). With AS_HEADERS,
# it writes <output>/<profile>/<basename>.sc.bin.h files containing C arrays
# (e.g. `vs_solid_essl[]`). bgfx::createEmbeddedShader then dispatches to the
# right one at runtime via BGFX_PLATFORM_SUPPORTS_* checks.
#
# When cross-compiling (emscripten), bgfx::shaderc is not built locally — the
# user must put a host-built shaderc on PATH first; we then synthesize an
# IMPORTED bgfx::shaderc target so bgfx_compile_shaders works.

if(NOT TARGET bgfx::shaderc)
    if(CMAKE_CROSSCOMPILING)
        find_program(NODEHAMMER_HOST_SHADERC shaderc
            DOC "Host-built bgfx shaderc (required for cross-compile shader builds)")
        if(NOT NODEHAMMER_HOST_SHADERC)
            message(FATAL_ERROR
                "NODEHAMMER_WITH_VIEWER is ON but bgfx::shaderc is not built and "
                "no host shaderc was found on PATH. Build the native preset first, "
                "then add the bgfx shaderc binary to PATH (it lands under "
                "build/.../_deps/bgfx-build/cmake/bgfx/) before configuring this preset.")
        endif()
        add_executable(bgfx::shaderc IMPORTED GLOBAL)
        set_property(TARGET bgfx::shaderc PROPERTY
            IMPORTED_LOCATION "${NODEHAMMER_HOST_SHADERC}")
    else()
        message(FATAL_ERROR
            "bgfx::shaderc target is not defined. bgfx.cmake should provide it "
            "when BGFX_BUILD_TOOLS=ON; check Dependencies.cmake.")
    endif()
endif()

set(NH_SHADER_VARYING ${CMAKE_CURRENT_SOURCE_DIR}/varying.def.sc)
set(NH_SHADER_OUTPUT ${CMAKE_BINARY_DIR}/shaders)

bgfx_compile_shaders(
    TYPE        VERTEX
    SHADERS     ${CMAKE_CURRENT_SOURCE_DIR}/vs_solid.sc
    VARYING_DEF ${NH_SHADER_VARYING}
    OUTPUT_DIR  ${NH_SHADER_OUTPUT}
    OUT_FILES_VAR NH_SHADER_VS_SOLID
    AS_HEADERS
)

bgfx_compile_shaders(
    TYPE        FRAGMENT
    SHADERS     ${CMAKE_CURRENT_SOURCE_DIR}/fs_solid.sc
    VARYING_DEF ${NH_SHADER_VARYING}
    OUTPUT_DIR  ${NH_SHADER_OUTPUT}
    OUT_FILES_VAR NH_SHADER_FS_SOLID
    AS_HEADERS
)

bgfx_compile_shaders(
    TYPE        VERTEX
    SHADERS     ${CMAKE_CURRENT_SOURCE_DIR}/vs_imgui.sc
    VARYING_DEF ${NH_SHADER_VARYING}
    OUTPUT_DIR  ${NH_SHADER_OUTPUT}
    OUT_FILES_VAR NH_SHADER_VS_IMGUI
    AS_HEADERS
)

bgfx_compile_shaders(
    TYPE        FRAGMENT
    SHADERS     ${CMAKE_CURRENT_SOURCE_DIR}/fs_imgui.sc
    VARYING_DEF ${NH_SHADER_VARYING}
    OUTPUT_DIR  ${NH_SHADER_OUTPUT}
    OUT_FILES_VAR NH_SHADER_FS_IMGUI
    AS_HEADERS
)

# Aggregate dependency target so consumers can add_dependencies() once.
add_custom_target(nodehammer_shaders DEPENDS
    ${NH_SHADER_VS_SOLID}
    ${NH_SHADER_FS_SOLID}
    ${NH_SHADER_VS_IMGUI}
    ${NH_SHADER_FS_IMGUI}
)

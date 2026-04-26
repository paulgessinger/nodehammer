# Shader build for the viewer (Stage 1: solid + imgui pipelines).
#
# bgfx_compile_shaders is provided by bgfxToolUtils.cmake (included from
# cmake/Dependencies.cmake when NODEHAMMER_WITH_VIEWER=ON). With AS_HEADERS,
# it writes <output>/<profile>/<basename>.sc.bin.h files containing C arrays
# (e.g. `vs_solid_essl[]`). bgfx::createEmbeddedShader then dispatches to the
# right one at runtime via BGFX_PLATFORM_SUPPORTS_* checks.
#
# bgfx::shaderc is resolved in cmake/Dependencies.cmake (typically via Conan
# tool_requires); if it isn't defined here something went wrong upstream.

if(NOT TARGET bgfx::shaderc)
    message(FATAL_ERROR
        "NODEHAMMER_WITH_VIEWER is ON but bgfx::shaderc is not defined. Either "
        "run `conan install . -o '&:viewer=True' ...` so the bgfx tool_requires "
        "puts shaderc on PATH, or install bgfx's shaderc system-wide.")
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

bgfx_compile_shaders(
    TYPE        VERTEX
    SHADERS     ${CMAKE_CURRENT_SOURCE_DIR}/vs_scene.sc
    VARYING_DEF ${NH_SHADER_VARYING}
    OUTPUT_DIR  ${NH_SHADER_OUTPUT}
    OUT_FILES_VAR NH_SHADER_VS_SCENE
    AS_HEADERS
)

bgfx_compile_shaders(
    TYPE        FRAGMENT
    SHADERS     ${CMAKE_CURRENT_SOURCE_DIR}/fs_scene_lambert.sc
    VARYING_DEF ${NH_SHADER_VARYING}
    OUTPUT_DIR  ${NH_SHADER_OUTPUT}
    OUT_FILES_VAR NH_SHADER_FS_SCENE_LAMBERT
    AS_HEADERS
)

# Aggregate dependency target so consumers can add_dependencies() once.
add_custom_target(nodehammer_shaders DEPENDS
    ${NH_SHADER_VS_SOLID}
    ${NH_SHADER_FS_SOLID}
    ${NH_SHADER_VS_IMGUI}
    ${NH_SHADER_FS_IMGUI}
    ${NH_SHADER_VS_SCENE}
    ${NH_SHADER_FS_SCENE_LAMBERT}
)

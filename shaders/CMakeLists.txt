# Shader build for the sokol viewer.
#
# nh_compile_shader (defined in cmake/Sokol.cmake) drives sokol-shdc on
# scene.glsl, producing a single C header that contains every requested
# backend's shader blob wrapped in #if defined(SOKOL_*) guards. Each viewer
# executable's backend define then selects exactly one variant at compile
# time — same .glsl input, no runtime cost, no per-target re-invocation.
#
# Slang choice:
#   Emscripten → glsl300es (WebGL2) + wgsl (WebGPU). The runtime-detect
#                shell loads whichever bundle the browser supports.
#   Apple      → metal_macos (primary) + glsl430 (fallback if Metal absent).
#   Linux      → glsl430.
#   Windows    → hlsl5 (primary) + glsl430.

if(EMSCRIPTEN)
    set(NH_SCENE_SLANGS "glsl300es;wgsl")
elseif(APPLE)
    set(NH_SCENE_SLANGS "metal_macos;glsl430")
elseif(WIN32)
    set(NH_SCENE_SLANGS "hlsl5;glsl430")
else()
    set(NH_SCENE_SLANGS "glsl430")
endif()

nh_compile_shader(scene.glsl
    OUT_HEADER       NH_SHADER_SCENE_HEADER
    SLANGS           ${NH_SCENE_SLANGS}
    DEPENDS_TARGET   nodehammer_shaders
)

# Expose the generated header's directory for consumers (the viewer lib).
set(NH_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
    CACHE INTERNAL "Directory containing sokol-shdc-generated headers")

# Validate that a shader's cross-compiled WGSL is well-formed under naga.
#
# The WebGPU backend cross-compiles the GLSL sources to WGSL, which the browser
# (Dawn/emdawnwebgpu) only compiles at runtime — so a bad WGSL cross-compile is
# invisible to the build and only errors in the browser. This script pulls that
# forward: it emits bare WGSL for the wgsl target via sokol-shdc, then runs each
# stage through the naga CLI (`cargo install naga-cli`), which parses and
# validates the module; a non-zero exit fails the build. Wired in from
# nh_compile_shader (cmake/Sokol.cmake) when NODEHAMMER_SHADER_STRICT is ON.
#
# naga is the wgpu reference validator, not Dawn/tint, so a pass is high
# confidence but not a Dawn guarantee — in practice they agree.
#
# Required defines:
#   -DSHDC=<sokol-shdc>  -DNAGA=<naga>  -DINPUT=<shader.glsl>
#   -DSTEM=<shader stem> -DTMPDIR=<scratch dir>

file(REMOVE_RECURSE "${TMPDIR}")
file(MAKE_DIRECTORY "${TMPDIR}")

# sokol-shdc resolves @include relative to the input file's directory.
execute_process(
    COMMAND "${SHDC}" -i "${INPUT}" -o "${TMPDIR}/${STEM}.h" -l wgsl -f bare
    RESULT_VARIABLE _shdc_rc
    OUTPUT_VARIABLE _shdc_out
    ERROR_VARIABLE  _shdc_err
)
if(NOT _shdc_rc EQUAL 0)
    message(FATAL_ERROR
        "WGSL validation: sokol-shdc failed for ${INPUT}\n${_shdc_out}${_shdc_err}")
endif()

file(GLOB _wgsl "${TMPDIR}/${STEM}.h_*.wgsl")
if(NOT _wgsl)
    message(FATAL_ERROR "WGSL validation: sokol-shdc emitted no WGSL for ${INPUT}")
endif()

set(_failures "")
foreach(_f IN LISTS _wgsl)
    # Convert WGSL -> WGSL: naga parses and validates the IR before writing, so
    # the round-trip is a full validation pass and a non-zero exit means invalid.
    execute_process(
        COMMAND "${NAGA}" "${_f}" "${_f}.out.wgsl"
        RESULT_VARIABLE _naga_rc
        OUTPUT_VARIABLE _naga_out
        ERROR_VARIABLE  _naga_err
    )
    if(NOT _naga_rc EQUAL 0)
        get_filename_component(_name "${_f}" NAME)
        string(APPEND _failures "  ── ${_name}\n${_naga_out}${_naga_err}\n")
    endif()
endforeach()

if(_failures)
    message(FATAL_ERROR
        "WGSL shader validation failed for ${STEM}.glsl under naga:\n${_failures}")
endif()

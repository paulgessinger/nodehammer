# Validate that a shader's cross-compiled HLSL is warning-clean under FXC.
#
# The D3D11 backend cross-compiles the GLSL sources to HLSL and compiles them
# with D3DCompile (FXC) at *runtime*, so FXC warnings (pow-of-negative,
# uninitialized returns, gradient-in-loop, …) never reach the normal build —
# they only surface as log spam when the viewer first loads a shader. This
# script pulls that check forward to build time: it emits bare HLSL for the
# hlsl5 target via sokol-shdc, then compiles every stage with `fxc /WX` so any
# warning fails the build. Wired in from nh_compile_shader (cmake/Sokol.cmake)
# when NODEHAMMER_SHADER_STRICT is ON.
#
# Required defines:
#   -DSHDC=<sokol-shdc>   -DFXC=<fxc.exe>   -DINPUT=<shader.glsl>
#   -DSTEM=<shader stem>  -DTMPDIR=<scratch dir>

file(REMOVE_RECURSE "${TMPDIR}")
file(MAKE_DIRECTORY "${TMPDIR}")

# sokol-shdc resolves @include relative to the input file's directory, so no
# extra include path is needed here.
execute_process(
    COMMAND "${SHDC}" -i "${INPUT}" -o "${TMPDIR}/${STEM}.h" -l hlsl5 -f bare
    RESULT_VARIABLE _shdc_rc
    OUTPUT_VARIABLE _shdc_out
    ERROR_VARIABLE  _shdc_err
)
if(NOT _shdc_rc EQUAL 0)
    message(FATAL_ERROR
        "shader HLSL validation: sokol-shdc failed for ${INPUT}\n${_shdc_out}${_shdc_err}")
endif()

file(GLOB _hlsl "${TMPDIR}/${STEM}.h_*.hlsl")
if(NOT _hlsl)
    message(FATAL_ERROR "shader HLSL validation: sokol-shdc emitted no HLSL for ${INPUT}")
endif()

set(_failures "")
foreach(_f IN LISTS _hlsl)
    # sokol-shdc names the emitted files ..._vertex.hlsl / ..._fragment.hlsl.
    set(_profile ps_5_0)
    if(_f MATCHES "_vertex\\.hlsl$")
        set(_profile vs_5_0)
    endif()
    execute_process(
        COMMAND "${FXC}" /nologo /T ${_profile} /E main /WX /Fo NUL "${_f}"
        RESULT_VARIABLE _fxc_rc
        OUTPUT_VARIABLE _fxc_out
        ERROR_VARIABLE  _fxc_err
    )
    if(NOT _fxc_rc EQUAL 0)
        get_filename_component(_name "${_f}" NAME)
        string(APPEND _failures "  ── ${_name}\n${_fxc_out}${_fxc_err}\n")
    endif()
endforeach()

if(_failures)
    message(FATAL_ERROR
        "HLSL shader validation failed for ${STEM}.glsl under fxc /WX "
        "(these would be D3DCompile warnings at viewer runtime):\n${_failures}")
endif()

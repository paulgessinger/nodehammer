# WGPU null-vertex-buffer workaround for sokol_gfx.h, applied as the sokol
# FetchContent PATCH_COMMAND (see cmake/Sokol.cmake). Runs via `cmake -P`, so
# it needs no external `sed` and works in any shell/CI environment.
#
# emdawnwebgpu's JS wrapper rejects setVertexBuffer(slot, null, 0, 0) — it
# requires a GPUBuffer object even though the WebGPU spec lists the buffer
# parameter as nullable. sokol_gfx already has the same workaround for
# setIndexBuffer (see the comment around line 17856 of sokol_gfx.h); apply the
# matching skip-when-null patch for setVertexBuffer here.
#
# Expects -Dsokol_gfx_h=<path to sokol_gfx.h>.

set(_orig "wgpuRenderPassEncoderSetVertexBuffer(_sg.wgpu.rpass_enc, slot, 0, 0, 0)")
set(_repl "((void)0)  /* nodehammer: emdawnwebgpu rejects null vb */")

file(READ "${sokol_gfx_h}" _src)

# Idempotent: PATCH_COMMAND can re-run if ExternalProject re-checks-out the
# source, so bail out cleanly when the replacement is already in place.
string(FIND "${_src}" "${_repl}" _already)
if(NOT _already EQUAL -1)
    message(STATUS "sokol_gfx.h: WGPU null-vb patch already applied")
    return()
endif()

string(REPLACE "${_orig}" "${_repl}" _patched "${_src}")
if(_patched STREQUAL _src)
    message(FATAL_ERROR
        "Failed to patch sokol_gfx.h for WGPU null-vb workaround: "
        "target call not found (did the pinned sokol commit change?)")
endif()

file(WRITE "${sokol_gfx_h}" "${_patched}")
message(STATUS "sokol_gfx.h: patched WGPU setVertexBuffer null-skip for emdawnwebgpu")

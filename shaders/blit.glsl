// Fullscreen 1:1 blit — samples a color texture and writes it straight into the
// current pass (the swapchain). Used by the "pause when static" path: a full
// frame renders the composite + ImGui into a persistent present-cache target,
// then this blit copies that cache into the swapchain. On paused frames only the
// blit runs, so the last full frame is re-presented cheaply — and, crucially, a
// valid frame is presented every time, which stops sokol's unconditional D3D11
// Present() from scanning out a stale flip-model back buffer (see App::Impl).
//
// VS reads NDC positions for a fullscreen triangle from a tiny static VBO owned
// by BlitPass — same rationale as composite.glsl (a real attribute-0 array
// avoids WebGL2's expensive gl_VertexIndex-synthesis path). Caller issues
// sg_draw(0, 3, 1) with that VBO bound at vertex_buffers[0].
//
// The V flip for top-left-origin backends is applied in the FS from a uniform
// flag, mirroring composite.glsl: the present-cache is an offscreen texture, so
// sampling it into the swapchain needs the same flip the composite applies when
// sampling the offscreen scene target.

@module blit

@vs blit_vs
in vec2 a_pos;
out vec2 v_uv;
void main() {
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
@end

@fs blit_fs
layout(binding=0) uniform blit_params {
    // x = flip_v (1.0 on top-left-origin backends like Metal/D3D/WGPU, 0.0 on
    //             GLCore/GLES3). yzw reserved.
    vec4 params;
};
layout(binding=0) uniform texture2D src;
layout(binding=0) uniform sampler smp;

in vec2 v_uv;
out vec4 frag_color;

void main() {
    vec2 uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, params.x));
    frag_color = texture(sampler2D(src, smp), uv);
}
@end

@program blit blit_vs blit_fs

// Fullscreen composite — samples the offscreen scene color (and optionally
// depth) into the swapchain. Today this is a passthrough; future HDR /
// tonemap / FXAA / dither work hangs off this shader's FS.
//
// Three modes selected by a uniform `mode`:
//   0 — color passthrough (default; visually identical to direct-to-swapchain)
//   1 — raw depth (closer fragments brighter under reversed-Z)
//   2 — linearized depth (uniform near→far gradient)
//
// VS uses gl_VertexID to synthesise a fullscreen triangle, so no vertex
// buffer or input attribute is required at draw time. Caller issues
// sg_draw(0, 3, 1).

@module composite

@vs composite_vs
out vec2 v_uv;
void main() {
    // gl_VertexIndex ∈ {0,1,2} → (0,0), (2,0), (0,2). The triangle covers
    // [-1,1]^2 in NDC; v_uv ∈ [0,2] across the triangle so the sampled
    // [0,1]^2 region tiles cleanly with no scaling math in the FS.
    // sokol-shdc uses Vulkan-flavored GLSL (gl_VertexIndex, not gl_VertexID);
    // it cross-compiles to gl_VertexID on GL backends.
    //
    // The V flip required for top-left-origin backends is applied in the
    // FS based on a uniform flag (see mode_near_far.w in composite_fs).
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs composite_fs
layout(binding=0) uniform composite_params {
    // x = mode (0=color, 1=raw depth, 2=linear depth)
    // y = near plane, z = far plane
    // w = flip_v (1.0 on top-left-origin backends like Metal/D3D/WGPU,
    //             0.0 on bottom-left-origin backends like GLCore/GLES3).
    //     Sokol's framebuffer NDC y is +up across all backends, but the
    //     texture origin convention differs — without this flag the
    //     composited image is upside-down on top-left-origin backends.
    vec4 mode_near_far;
};

layout(binding=0) uniform texture2D scene_color;
// WebGPU rejects the default Float/filterable sample type for a Depth32Float
// texture. Mark depth as unfilterable_float and pair with a nonfiltering
// sampler (depth visualizations don't need bilinear filtering anyway).
@image_sample_type scene_depth unfilterable_float
layout(binding=1) uniform texture2D scene_depth;
layout(binding=0) uniform sampler smp_color;
@sampler_type smp_depth nonfiltering
layout(binding=1) uniform sampler smp_depth;

in vec2 v_uv;
out vec4 frag_color;

// Reversed-Z linearization. Under reversed-Z + [0,1] depth range, the
// sampled depth is 1 at the near plane and 0 at the far plane. The
// algebra below collapses to the same closed form regardless of GL's
// [-1,1] vs Metal/D3D/WGPU [0,1] clip-space depth, because the depth
// *texture* always contains [0,1] values after driver normalization.
float linearize_reversed_z(float d, float n, float f) {
    return (n * f) / (n + d * (f - n));
}

void main() {
    int mode = int(mode_near_far.x);
    float flip_v = mode_near_far.w;
    vec2 uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, flip_v));
    if (mode == 1) {
        float d = texture(sampler2D(scene_depth, smp_depth), uv).r;
        frag_color = vec4(vec3(d), 1.0);
    } else if (mode == 2) {
        float n = mode_near_far.y;
        float f = mode_near_far.z;
        float d = texture(sampler2D(scene_depth, smp_depth), uv).r;
        float zv = linearize_reversed_z(d, n, f);
        float t = clamp((zv - n) / (f - n), 0.0, 1.0);
        frag_color = vec4(vec3(t), 1.0);
    } else {
        frag_color = texture(sampler2D(scene_color, smp_color), uv);
    }
}
@end

@program composite composite_vs composite_fs

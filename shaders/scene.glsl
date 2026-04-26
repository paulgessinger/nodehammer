// Scene shader for the sokol viewer. Annotated GLSL — sokol-shdc parses the
// @vs / @fs / @program blocks below and emits a generated C header with
// shader source / bytecode for every requested target language plus
// reflection structs (sg_shader_desc, attribute slots, uniform-block sizes).
//
// One uniform block per stage — sokol_gfx wants vs and fs uniforms in
// separate blocks. The vs block carries the camera matrix; the fs block
// carries the per-draw material colour and the global light direction.
//
// Per-instance world matrix lives in vertex attributes inst0..inst3 with
// step rate "instance". scene_renderer.cpp populates a second vertex buffer
// of mat4s and binds it to slot 1.

@module scene

@vs scene_vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;
};

in vec3 a_position;
in vec3 a_normal;
in vec4 inst0;
in vec4 inst1;
in vec4 inst2;
in vec4 inst3;

out vec3 v_normal_world;

void main() {
    vec4 world_pos =
          a_position.x * inst0
        + a_position.y * inst1
        + a_position.z * inst2
        +                inst3;
    gl_Position = view_proj * world_pos;

    v_normal_world =
          a_normal.x * inst0.xyz
        + a_normal.y * inst1.xyz
        + a_normal.z * inst2.xyz;
}
@end

@fs scene_fs
layout(binding=1) uniform fs_params {
    vec4 base_color;   // rgb = albedo, a = opacity
    vec4 light_dir;    // xyz = direction TO light, world space
};

in vec3 v_normal_world;
out vec4 frag_color;

void main() {
    vec3 n = normalize(v_normal_world);
    // Two-sided shading: detector inner/outer faces should both light up
    // regardless of winding. Mirrors the bgfx fs_scene_lambert behaviour.
    float ndl = max(abs(dot(n, -light_dir.xyz)), 0.0);
    vec3 rgb = base_color.rgb * (0.20 + 0.80 * ndl);
    frag_color = vec4(rgb, base_color.a);
}
@end

@program scene scene_vs scene_fs

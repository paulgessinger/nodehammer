// Scene shader for the sokol viewer. Annotated GLSL — sokol-shdc parses the
// @vs / @fs / @program blocks below and emits a generated C header with
// shader source / bytecode for every requested target language plus
// reflection structs (sg_shader_desc, attribute slots, uniform-block sizes).
//
// One uniform block per stage — sokol_gfx wants vs and fs uniforms in
// separate blocks. The vs block carries the camera matrix; the fs block
// carries the per-draw material colour, global light direction, and optional
// azimuthal cut parameters.
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
out vec3 v_world_pos;

void main() {
    vec4 world_pos =
          a_position.x * inst0
        + a_position.y * inst1
        + a_position.z * inst2
        +                inst3;
    v_world_pos = world_pos.xyz;
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
    vec4 cut_params;   // x = enabled, y/z = start/end phi in radians
};

in vec3 v_normal_world;
in vec3 v_world_pos;
out vec4 frag_color;

bool angle_in_cut(float phi, float start_phi, float end_phi) {
    const float tau = 6.283185307179586;
    if (phi < 0.0) {
        phi += tau;
    }
    if (start_phi < 0.0) {
        start_phi += tau;
    }
    if (end_phi < 0.0) {
        end_phi += tau;
    }
    if (abs(start_phi - end_phi) < 0.0001) {
        return false;
    }
    if (start_phi <= end_phi) {
        return phi >= start_phi && phi <= end_phi;
    }
    return phi >= start_phi || phi <= end_phi;
}

void main() {
    if (cut_params.x > 0.5) {
        float phi = atan(v_world_pos.y, v_world_pos.x);
        if (angle_in_cut(phi, cut_params.y, cut_params.z)) {
            discard;
        }
    }

    vec3 n = normalize(v_normal_world);
    // Two-sided shading: detector inner/outer faces should both light up
    // regardless of winding. Mirrors the bgfx fs_scene_lambert behaviour.
    float ndl = max(abs(dot(n, -light_dir.xyz)), 0.0);
    vec3 rgb = base_color.rgb * (0.20 + 0.80 * ndl);
    frag_color = vec4(rgb, base_color.a);
}
@end

@program scene scene_vs scene_fs

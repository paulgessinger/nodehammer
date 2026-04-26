$input  a_position, a_normal, i_data0, i_data1, i_data2, i_data3
$output v_normal_world

#include <bgfx_shader.sh>

// Apply the per-instance world matrix as an explicit column expansion:
// world = pos.x * col0 + pos.y * col1 + pos.z * col2 + col3
// This sidesteps any platform-specific quirk in `mat4 m; m[i] = i_data_i`
// (HLSL/Metal sometimes treat that assignment differently from GLSL).
void main()
{
    vec4 world_pos =
          a_position.x * i_data0
        + a_position.y * i_data1
        + a_position.z * i_data2
        +                i_data3;

    gl_Position = mul(u_viewProj, world_pos);

    vec3 world_normal =
          a_normal.x * i_data0.xyz
        + a_normal.y * i_data1.xyz
        + a_normal.z * i_data2.xyz;
    v_normal_world = world_normal;
}

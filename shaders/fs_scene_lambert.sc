$input v_normal_world

#include <bgfx_shader.sh>

uniform vec4 u_baseColor;     // rgb = albedo, a = opacity
uniform vec4 u_lightDir;      // xyz = direction to light, world space (normalised)

void main()
{
    vec3 n = normalize(v_normal_world);
    // Two-sided shading so back-facing detector parts aren't black.
    float ndl = max(abs(dot(n, -u_lightDir.xyz)), 0.0);
    vec3  rgb = u_baseColor.rgb * (0.20 + 0.80 * ndl);
    gl_FragColor = vec4(rgb, u_baseColor.a);
}

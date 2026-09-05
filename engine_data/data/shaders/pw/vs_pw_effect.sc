$input a_position, a_texcoord0, a_color0, a_texcoord1
$output v_texcoord0, v_color0, v_warpStrength, v_clipPosition

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    v_warpStrength = a_texcoord1;
    v_clipPosition = gl_Position;
}

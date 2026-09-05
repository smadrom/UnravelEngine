$input v_texcoord0, v_color0, v_warpStrength, v_clipPosition

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_shaderTexture, 1);
SAMPLER2D(s_sceneColor, 2);
uniform vec4 u_pwEffectMode;
uniform vec4 u_pwFlowOffset;
uniform vec4 u_pwFlowBlend;

void main()
{
    vec4 base = texture2D(s_texColor, v_texcoord0);
    vec4 color;
    if(u_pwEffectMode.x > 1.5)
    {
        // Angelica fluid.txt: mask coordinates come from diffuse RG and animated C0.
        color = mix(base, texture2D(s_shaderTexture, base.rg + u_pwFlowOffset.rg), u_pwFlowBlend.r);
    }
    else if(u_pwEffectMode.x > 0.5)
    {
        if(base.a < 0.05 || dot(v_warpStrength, v_warpStrength) <= 0.0) discard;
        vec2 screen = v_clipPosition.xy / v_clipPosition.w * 0.5 + 0.5;
        vec2 displacement = (base.rg * 2.0 - 1.0) * vec2(-1.0, 1.0) * v_warpStrength * base.a;
        if(u_pwEffectMode.y < 0.5) screen.y = 1.0 - screen.y;
        else displacement.y = -displacement.y;
        color = vec4(texture2D(s_sceneColor, screen + displacement).rgb, 1.0);
    }
    else
    {
        // The draw state applies the authored blend factors without premultiplication.
        color = base * v_color0;
    }
    if(u_pwEffectMode.z >= 0.0 && color.a < u_pwEffectMode.z) discard;
    gl_FragColor = color;
}

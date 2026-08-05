#version 450

#define FOG_NONE 0
#define FOG_LINEAR 1
#define FOG_EXP 2
#define LOG2 1.442695

layout(set = 3, binding = 0) uniform ModelFragmentParameters
{
    int   clipplanes;
    float alpha;
    float alpha_test;
    float additive_pass;

    int   fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;

    vec4  fog_color;
};

layout(set = 2, binding = 0) uniform sampler2D tex0;

layout(location = 0) out vec4 frag_color;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 vpos;
layout(location = 3) in float clipvertex0;
layout(location = 4) in float clipvertex1;
layout(location = 5) in float clipvertex2;
layout(location = 6) in float clipvertex3;
layout(location = 7) in float clipvertex4;
layout(location = 8) in float clipvertex5;

float FogFactor(float fog_dist)
{
    if (fog_mode == FOG_NONE)
    {
        return 0.0;
    }

    if (fog_mode == FOG_LINEAR)
    {
        return clamp(smoothstep(fog_start, fog_end, fog_dist), 0.0, 1.0);
    }

    return 1.0 - clamp(exp2(-fog_density * fog_density * fog_dist * fog_dist * LOG2), 0.0, 1.0);
}

void main()
{
    float c = 0.0;

    if ((clipplanes & 1) == 1)
    {
        c += min(0.0, clipvertex0);
    }
    if ((clipplanes & 2) == 2)
    {
        c += min(0.0, clipvertex1);
    }
    if ((clipplanes & 4) == 4)
    {
        c += min(0.0, clipvertex2);
    }
    if ((clipplanes & 8) == 8)
    {
        c += min(0.0, clipvertex3);
    }
    if ((clipplanes & 16) == 16)
    {
        c += min(0.0, clipvertex4);
    }
    if ((clipplanes & 32) == 32)
    {
        c += min(0.0, clipvertex5);
    }

    if (c < 0.0)
    {
        discard;
    }

    vec4 texel = texture(tex0, uv);

    if (alpha_test != 0.0 && texel.w < alpha_test)
    {
        discard;
    }

    vec3 rgb = mix(texel.rgb * color, color, additive_pass);

    float fogf = FogFactor(length(vpos));

    if (fogf > 0.0)
    {
        rgb = mix(rgb, fog_color.rgb, fogf);
    }

    frag_color = vec4(rgb, texel.w * alpha);
}

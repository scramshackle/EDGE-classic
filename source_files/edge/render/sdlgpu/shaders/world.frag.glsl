#version 450

#define FOG_NONE 0
#define FOG_LINEAR 1
#define FOG_EXP 2
#define LOG2 1.442695

layout(set = 3, binding = 0) uniform FragmentParameters
{
    int   flags;
    float alpha_test;
    int   clipplanes;
    int   fog_mode;
    vec4  fog_color;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_scale;
};

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;

layout(location = 0) out vec4 frag_color;

layout(location = 0) in vec4 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in vec3 vpos;
layout(location = 3) in float clipvertex0;
layout(location = 4) in float clipvertex1;
layout(location = 5) in float clipvertex2;
layout(location = 6) in float clipvertex3;
layout(location = 7) in float clipvertex4;
layout(location = 8) in float clipvertex5;

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

    if ((flags & 2) == 2)
    {
        float u           = uv[0];
        float v           = uv[1];
        float line_width  = uv[2];
        float line_length = uv[3];
        float aa_radius   = 1.0;

        float au = 1.0 - smoothstep(1.0 - ((3.0 * aa_radius) / line_width), 1.0, abs(u / line_width));
        float av = 1.0 - smoothstep(1.0 - ((3.0 * aa_radius) / line_length), 1.0, abs(v / line_length));

        frag_color = color;
        frag_color.a *= min(au, av);
        return;
    }

    vec4 c0 = texture(tex0, uv.xy);
    if (alpha_test != 0.0 && c0.w < alpha_test)
    {
        discard;
    }

    vec4 fcolor = color;

    float fogf = 0.0;
    if (fog_mode != FOG_NONE)
    {
        float fog_dist = length(vpos);

        if (fog_mode == FOG_LINEAR)
        {
            fogf = clamp(smoothstep(fog_start, fog_end, fog_dist), 0.0, 1.0);
        }
        else
        {
            fogf = 1.0 - clamp(exp2(-fog_density * fog_density * fog_dist * fog_dist * LOG2), 0.0, 1.0);
        }
    }

    if ((flags & 1) == 1)
    {
        fcolor *= c0;
        fcolor *= texture(tex1, uv.zw);

        if (fogf > 0.0)
        {
            fcolor = mix(fcolor, fog_color, fogf);
        }
    }
    else
    {
        fcolor *= c0;
        if (fogf > 0.0)
        {
            fcolor.rgb = mix(fcolor, fog_color, fogf).rgb;
        }
    }

    frag_color = fcolor;
}

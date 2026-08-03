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

    mat4  sky_inverse_projection;
    mat4  sky_inverse_view;
    vec4  sky_viewport;
    float sky_stretch_mode;
    float sky_h_ratio;
    float sky_u_scale;
    float sky_ty;
    float sky_u_offset;
    float sky_v_offset;
    float sky_vertical_fov_slope;
    float sky_horizon_shift;
};

#define SKY_STRETCH_MIRROR 0.0
#define SKY_STRETCH_REPEAT 1.0
#define SKY_STRETCH_STRETCH 2.0
#define SKY_STRETCH_VANILLA 3.0

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

vec4 SampleEquirectSky()
{
    vec2 ndc  = ((gl_FragCoord.xy - sky_viewport.xy) / sky_viewport.zw) * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);
    vec4 eye  = sky_inverse_projection * clip;
    eye       = vec4(eye.xy, -1.0, 0.0);
    vec3 dir  = normalize((sky_inverse_view * eye).xyz);

    const float kTwoPi = 6.28318530718;

    float horiz_len = max(length(dir.xy), 0.0001);
    float sky_tan   = dir.z / horiz_len - sky_horizon_shift;
    float p         = sky_tan / sqrt(1.0 + sky_tan * sky_tan);
    float u = fract((atan(dir.y, dir.x) / kTwoPi + 0.5) * sky_u_scale + sky_u_offset);

    bool mode_is_vanilla = abs(sky_stretch_mode - SKY_STRETCH_VANILLA) < 0.5;
    bool mode_is_mirror  = abs(sky_stretch_mode - SKY_STRETCH_MIRROR) < 0.5;
    bool lower_hemisphere = p < 0.0;

    float v_raw;

    if (mode_is_vanilla)
    {
        float pc = clamp(sky_tan / sky_vertical_fov_slope, -1.0, 1.0);
        v_raw           = (pc + 1.0) * 0.5 * sky_h_ratio * sky_ty - sky_v_offset;
    }
    else if (mode_is_mirror)
    {
        float base = (p + 1.0) * 0.5 * sky_ty;
        v_raw      = lower_hemisphere ? -(base + sky_v_offset) : (base - sky_v_offset);
    }
    else
    {
        v_raw = (p + 1.0) * 0.5 * sky_ty - sky_v_offset;
    }

    float v = fract(v_raw);

    vec4 sampled = texture(tex0, vec2(u, v));

    vec3 rgb = sampled.rgb * color.rgb;

    float fogf = FogFactor(length(vpos));

    if (fogf > 0.0)
    {
        rgb = mix(rgb, fog_color.rgb, fogf);
    }

    return vec4(rgb, sampled.a * color.a);
}

void main()
{
    if ((flags & 8) == 8)
    {
        frag_color = SampleEquirectSky();
        return;
    }

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

    if ((flags & 4) == 4)
    {
        c0.rgb = vec3(1.0);
    }

    vec4 fcolor = color;

    float fogf = FogFactor(length(vpos));

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

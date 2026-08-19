#version 450

#define FOG_NONE 0
#define FOG_LINEAR 1
#define FOG_EXP 2
#define LOG2 1.442695

layout(set = 3, binding = 0) uniform ModelFragmentParameters
{
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

#ifdef EDGE_OIT_PASS
layout(location = 1) out vec4 frag_revealage;

float OitWeight(float a_alpha, float view_depth)
{
    float a = min(1.0, a_alpha * 10.0) + 0.01;
    float d = 1.0 - view_depth * 0.9;

    return clamp(a * a * a * 1e8 * d * d * d, 1e-2, 3e3);
}

void OitEmit(vec4 fragment_color)
{
    float view_depth = clamp(-vpos.z * 0.000625, 0.0, 1.0);
    float weight     = OitWeight(fragment_color.a, view_depth);

    frag_color     = vec4(fragment_color.rgb * fragment_color.a, fragment_color.a) * weight;
    frag_revealage = vec4(fragment_color.a);
}
#endif

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

    vec4 fcolor = vec4(rgb, texel.w * alpha);

#ifdef EDGE_OIT_PASS
    OitEmit(fcolor);
#else
    frag_color = fcolor;
#endif
}

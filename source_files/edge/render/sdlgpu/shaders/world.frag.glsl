#version 450

#define FOG_NONE 0
#define FOG_LINEAR 1
#define FOG_EXP 2
#define LOG2 1.442695

layout(set = 3, binding = 0) uniform FragmentParameters
{
    int   flags;
    float alpha_test;
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
    float sky_u_scale;
    float sky_ty;
    float sky_u_offset;
    float sky_v_offset;
    float sky_vertical_fov_slope;
    float sky_horizon_shift;
    float sky_is_box;

    vec4 light_view;
    vec4 light_range;

    vec4 glow_plane[2];
    vec4 glow_color[2];
    vec4 glow_additive;
};


#define SKY_STRETCH_MIRROR 0.0
#define SKY_STRETCH_REPEAT 1.0
#define SKY_STRETCH_STRETCH 2.0
#define SKY_STRETCH_VANILLA 3.0

#define SKY_PINCH_TAPS 64
#define SKY_PINCH_TAP_WEIGHT 0.015625
#define SKY_PINCH_FADE_START 0.9397
#define SKY_PINCH_FADE_END 0.9848

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform samplerCube texCube;

layout(location = 0) out vec4 frag_color;

layout(location = 0) in vec4 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in vec3 vpos;

struct GpuLight
{
    vec4 position_radius;
    vec4 color_additive;
};

layout(set = 2, binding = 3) readonly buffer LightBuffer
{
    GpuLight gpu_lights[];
};

layout(set = 2, binding = 4) readonly buffer ClusterBuffer
{
    uint gpu_clusters[];
};

layout(set = 2, binding = 5) readonly buffer LightIndexBuffer
{
    uint gpu_light_indices[];
};

const float kLightTileSize = 16.0;
const int   kLightSlices   = 8;

void AccumulateClusterLights(inout vec3 modulate_sum, inout vec3 additive_sum)
{
    float tile_x = floor((gl_FragCoord.x - light_view.x) / kLightTileSize);
    float tile_y = floor((light_view.y - gl_FragCoord.y) / kLightTileSize);

    if (tile_x < 0.0 || tile_y < 0.0 || tile_x >= light_view.z || tile_y >= light_view.w)
        return;

    float depth = max(-vpos.z, light_range.x);

    float ratio = log(depth / light_range.x) / log(light_range.y / light_range.x);

    int slice = clamp(int(ratio * float(kLightSlices)), 0, kLightSlices - 1);

    int cluster = int(light_range.w) + (slice * int(light_view.w) + int(tile_y)) * int(light_view.z) + int(tile_x);

    uint entry  = gpu_clusters[cluster];
    uint offset = entry >> 8u;
    uint count  = entry & 255u;

    for (uint i = 0u; i < count; i++)
    {
        GpuLight light = gpu_lights[gpu_light_indices[offset + i]];

        vec3 delta = light.position_radius.xyz - vpos;

        float radius = light.position_radius.w;

        float normalised = dot(delta, delta) / (radius * radius);

        if (normalised >= 1.0)
            continue;

        vec3 contribution = light.color_additive.rgb * exp(-5.44 * normalised);

        if (light.color_additive.a > 0.5)
            additive_sum += contribution;
        else
            modulate_sum += contribution;
    }
}

void AccumulateGlows(inout vec3 modulate_sum, inout vec3 additive_sum)
{
    for (int index = 0; index < 2; index++)
    {
        if (float(index) >= glow_additive.w)
            break;

        float plane_distance = dot(vec4(vpos, 1.0), glow_plane[index]);

        float radius = glow_color[index].a;

        float normalised = (plane_distance * plane_distance) / (radius * radius);

        if (normalised >= 1.0)
            continue;

        vec3 contribution = glow_color[index].rgb * exp(-5.44 * normalised);

        if (glow_additive[index] > 0.5)
            additive_sum += contribution;
        else
            modulate_sum += contribution;
    }
}

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

vec3 SkyDirection()
{
    vec2 ndc  = ((gl_FragCoord.xy - sky_viewport.xy) / sky_viewport.zw) * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);
    vec4 eye  = sky_inverse_projection * clip;
    eye       = vec4(eye.xy, -1.0, 0.0);
    return normalize((sky_inverse_view * eye).xyz);
}

vec4 SampleCubeSky()
{
    vec3 dir = SkyDirection();

    vec4 sampled = texture(texCube, vec3(dir.x, dir.z, -dir.y));

    vec3 rgb = sampled.rgb * color.rgb;

    float fogf = FogFactor(length(vpos));

    if (fogf > 0.0)
    {
        rgb = mix(rgb, fog_color.rgb, fogf);
    }

    return vec4(rgb, sampled.a * color.a);
}

vec4 SampleEquirectSky()
{
    vec3 dir = SkyDirection();

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
        v_raw           = (pc + 1.0) * 0.5 * sky_ty - sky_v_offset;
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

    float pinch_mix = smoothstep(SKY_PINCH_FADE_START, SKY_PINCH_FADE_END, abs(dir.z));

    if (pinch_mix > 0.0)
    {
        vec4 averaged = vec4(0.0);

        float average_span = min(sky_u_scale, 1.0);

        for (int i = 0; i < SKY_PINCH_TAPS; i++)
        {
            float tap = (float(i) + 0.5) * SKY_PINCH_TAP_WEIGHT - 0.5;

            averaged += texture(tex0, vec2(fract(u + tap * average_span), v));
        }

        sampled = mix(sampled, averaged * SKY_PINCH_TAP_WEIGHT, pinch_mix);
    }

    vec3 rgb = sampled.rgb * color.rgb;

    float fogf = FogFactor(length(vpos));

    if (fogf > 0.0)
    {
        rgb = mix(rgb, fog_color.rgb, fogf);
    }

    return vec4(rgb, sampled.a * color.a);
}

#ifdef EDGE_OIT_PASS
layout(location = 1) out vec4 frag_revealage;

float OitWeight(float alpha, float view_depth)
{
    float a = min(1.0, alpha * 10.0) + 0.01;
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

void main()
{
    if ((flags & 16) == 16)
    {
        vec4  accumulation = texture(tex0, uv.xy);
        float revealage    = texture(tex1, uv.xy).a;

        float weight = max(accumulation.a, 1e-5);

        frag_color = vec4(accumulation.rgb / weight, 1.0 - revealage);
        return;
    }

    if ((flags & 8) == 8)
    {
        if (sky_is_box > 0.5)
            frag_color = SampleCubeSky();
        else
            frag_color = SampleEquirectSky();
        return;
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

        vec4 line_color = color;
        line_color.a *= min(au, av);
#ifdef EDGE_OIT_PASS
        OitEmit(line_color);
#else
        frag_color = line_color;
#endif
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

    vec3 modulate_sum = vec3(0.0);
    vec3 additive_sum = vec3(0.0);

    if (light_range.z > 0.5)
    {
        AccumulateClusterLights(modulate_sum, additive_sum);
    }

    if (glow_additive.w > 0.5)
    {
        AccumulateGlows(modulate_sum, additive_sum);
    }

    if ((flags & 1) == 1 || (flags & 32) == 32)
    {
        fcolor *= c0;

        if ((flags & 32) == 32)
        {
            vec2 falloff = uv.zw * 2.0 - 1.0;

            fcolor.rgb *= exp(-5.44 * dot(falloff, falloff));
        }
        else
        {
            fcolor *= texture(tex1, uv.zw);
        }

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

    fcolor.rgb += c0.rgb * modulate_sum + additive_sum;

#ifdef EDGE_OIT_PASS
    OitEmit(fcolor);
#else
    frag_color = fcolor;
#endif
}

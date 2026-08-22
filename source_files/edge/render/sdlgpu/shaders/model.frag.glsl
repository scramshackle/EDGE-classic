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

    vec4  light_view;
    vec4  light_range;

    vec4  glow_plane[2];
    vec4  glow_color[2];
    vec4  glow_additive;
};

struct GpuLight
{
    vec4 position_radius;
    vec4 color_additive;
};

layout(set = 2, binding = 1) readonly buffer LightBuffer
{
    GpuLight gpu_lights[];
};

layout(set = 2, binding = 2) readonly buffer ClusterBuffer
{
    uint gpu_clusters[];
};

layout(set = 2, binding = 3) readonly buffer LightIndexBuffer
{
    uint gpu_light_indices[];
};

layout(set = 2, binding = 0) uniform sampler2D tex0;

layout(location = 0) out vec4 frag_color;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 vpos;
layout(location = 3) in vec3 vnormal;

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

    vec3 surface_normal = normalize(vnormal);

    for (uint i = 0u; i < count; i++)
    {
        GpuLight light = gpu_lights[gpu_light_indices[offset + i]];

        vec3 delta = light.position_radius.xyz - vpos;

        float radius = light.position_radius.w;

        float normalised = dot(delta, delta) / (radius * radius);

        if (normalised >= 1.0)
            continue;

        float shade = max(0.6 - 0.7 * dot(normalize(-delta), surface_normal), 0.0);

        vec3 contribution = light.color_additive.rgb * exp(-5.44 * normalised) * shade;

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

    vec3 rgb = mix(texel.rgb * color, color, additive_pass);

    rgb += (texel.rgb * modulate_sum + additive_sum) * (1.0 - additive_pass);

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

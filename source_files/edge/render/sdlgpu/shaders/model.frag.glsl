#version 450

#define FOG_NONE 0
#define FOG_LINEAR 1
#define FOG_EXP 2
#define LOG2 1.442695

layout(set = 3, binding = 0) uniform ModelFragmentParameters
{
    float alpha;
    float alpha_test;
    float model_fragment_padding1;

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

    vec4  color_lookup;
};

struct GpuLight
{
    vec4 position_radius;
    vec4 color_additive;
};

#ifdef EDGE_OIT_PASS
#define EDGE_MODEL_STORAGE_BINDING 3
#else
#define EDGE_MODEL_STORAGE_BINDING 2
#endif

layout(set = 2, binding = EDGE_MODEL_STORAGE_BINDING) readonly buffer LightBuffer
{
    GpuLight gpu_lights[];
};

layout(set = 2, binding = EDGE_MODEL_STORAGE_BINDING + 1) readonly buffer ClusterBuffer
{
    uint gpu_clusters[];
};

layout(set = 2, binding = EDGE_MODEL_STORAGE_BINDING + 2) readonly buffer LightIndexBuffer
{
    uint gpu_light_indices[];
};

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler3D color_lookup_texture;

#ifdef EDGE_OIT_PASS
layout(set = 2, binding = 2) uniform sampler2D oit_front_depth;
#endif

layout(location = 0) out vec4 frag_color;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 vpos;
layout(location = 3) in vec3 vnormal;

const float kLightClusterSize = 16.0;
const int   kLightSlices   = 8;

void AccumulateClusterLights(inout vec3 modulate_sum, inout vec3 additive_sum)
{
    float cluster_x = floor((gl_FragCoord.x - light_view.x) / kLightClusterSize);
    float cluster_y = floor((light_view.y - gl_FragCoord.y) / kLightClusterSize);

    if (cluster_x < 0.0 || cluster_y < 0.0 || cluster_x >= light_view.z || cluster_y >= light_view.w)
        return;

    float depth = max(-vpos.z, light_range.x);

    float ratio = log(depth / light_range.x) / log(light_range.y / light_range.x);

    int slice = clamp(int(ratio * float(kLightSlices)), 0, kLightSlices - 1);

    int cluster =
        int(light_range.w) + (slice * int(light_view.w) + int(cluster_y)) * int(light_view.z) + int(cluster_x);

    uint entry  = gpu_clusters[cluster];
    uint offset = entry >> 8u;
    uint count  = entry & 255u;

    vec3 surface_normal = normalize(vnormal);

    for (uint i = 0u; i < count; i++)
    {
        uint index_entry = offset + i;
        uint local_index = (gpu_light_indices[index_entry >> 1u] >> ((index_entry & 1u) * 16u)) & 65535u;

        GpuLight light = gpu_lights[uint(light_range.z) - 1u + local_index];

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
const float kOitFrontClear     = 65504.0;
const float kOitFrontTolerance = 1.0 + 1.0 / 512.0;

#ifndef EDGE_OIT_FRONT_DEPTH
layout(location = 1) out vec4 frag_revealage;
layout(location = 2) out vec4 frag_front;

float OitWeight(float alpha, float view_distance)
{
    float d = 32.0 / max(view_distance, 32.0);

    return alpha * clamp(3e3 * d * d * d, 1e-2, 3e3);
}

void OitEmit(vec4 fragment_color)
{
    float view_distance = -vpos.z;
    float front         = texelFetch(oit_front_depth, ivec2(gl_FragCoord.xy), 0).r;

    if (view_distance <= front * kOitFrontTolerance)
    {
        frag_color     = vec4(0.0);
        frag_revealage = vec4(0.0);
        frag_front     = vec4(fragment_color.rgb * fragment_color.a, fragment_color.a);
        return;
    }

    float weight = OitWeight(fragment_color.a, view_distance);

    frag_color     = vec4(fragment_color.rgb * fragment_color.a, fragment_color.a) * weight;
    frag_revealage = vec4(fragment_color.a);
    frag_front     = vec4(0.0);
}
#endif
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

vec3 ApplyColorLookup(vec3 source)
{
    vec3 coordinate = clamp(source, 0.0, 1.0) * (63.0 / 64.0) + (0.5 / 64.0);

    return texture(color_lookup_texture, coordinate).rgb;
}

void main()
{
    vec4 texel = texture(tex0, uv);

    if (alpha_test != 0.0 && texel.w < alpha_test)
    {
        discard;
    }

#ifdef EDGE_OIT_FRONT_DEPTH
    frag_color = vec4(texel.w * alpha < 1.0 / 255.0 ? kOitFrontClear : -vpos.z);
    return;
#endif

    if (color_lookup.x > 0.5)
    {
        texel.rgb = ApplyColorLookup(texel.rgb);
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

    vec3 rgb = texel.rgb * color;

    rgb += texel.rgb * modulate_sum + additive_sum;

    float fogf = FogFactor(length(vpos));

    if (fogf > 0.0)
    {
        rgb = mix(rgb, fog_color.rgb, fogf);
    }

    vec4 fcolor = vec4(rgb, texel.w * alpha);

#if defined(EDGE_OIT_FRONT_DEPTH)
#elif defined(EDGE_OIT_PASS)
    OitEmit(fcolor);
#else
    frag_color = fcolor;
#endif
}

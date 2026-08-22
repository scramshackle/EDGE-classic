#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct GpuLight
{
    vec4 position_radius;
    vec4 color_additive;
};

layout(set = 0, binding = 0) readonly buffer LightBuffer
{
    GpuLight gpu_lights[];
};

layout(set = 1, binding = 0) buffer ClusterBuffer
{
    uint gpu_clusters[];
};

layout(set = 1, binding = 1) buffer LightIndexBuffer
{
    uint gpu_light_indices[];
};

layout(set = 1, binding = 2) buffer LightCounterBuffer
{
    uint gpu_light_counter[];
};

layout(set = 2, binding = 0) uniform CullParameters
{
    vec4  frustum;
    vec4  viewport;
    uvec4 grid;
    uvec4 range;
};

const uint kMaximumPerCluster = 64u;

void main()
{
    uint cluster = gl_GlobalInvocationID.x;

    uint tiles_x = grid.x;
    uint tiles_y = grid.y;
    uint slices  = grid.z;

    uint cluster_total = tiles_x * tiles_y * slices;

    if (cluster >= cluster_total)
        return;

    uint tile_x = cluster % tiles_x;
    uint tile_y = (cluster / tiles_x) % tiles_y;
    uint slice  = cluster / (tiles_x * tiles_y);

    float near_plane = frustum.z;
    float far_plane  = frustum.w;

    float ratio = far_plane / near_plane;

    float depth_near = near_plane * pow(ratio, float(slice) / float(slices));
    float depth_far  = near_plane * pow(ratio, float(slice + 1u) / float(slices));

    depth_near *= 0.99;
    depth_far *= 1.01;

    float tile_size = viewport.z;

    float ndc_x0 = (float(tile_x) * tile_size) / viewport.x * 2.0 - 1.0;
    float ndc_x1 = (float(tile_x + 1u) * tile_size) / viewport.x * 2.0 - 1.0;
    float ndc_y0 = (float(tile_y) * tile_size) / viewport.y * 2.0 - 1.0;
    float ndc_y1 = (float(tile_y + 1u) * tile_size) / viewport.y * 2.0 - 1.0;

    float flip = viewport.w;

    ndc_x0 *= flip;
    ndc_x1 *= flip;

    float x_slope = frustum.x;
    float y_slope = frustum.y;

    float xa = ndc_x0 * x_slope * depth_near;
    float xb = ndc_x1 * x_slope * depth_near;
    float xc = ndc_x0 * x_slope * depth_far;
    float xd = ndc_x1 * x_slope * depth_far;

    float ya = ndc_y0 * y_slope * depth_near;
    float yb = ndc_y1 * y_slope * depth_near;
    float yc = ndc_y0 * y_slope * depth_far;
    float yd = ndc_y1 * y_slope * depth_far;

    vec3 bounds_minimum = vec3(min(min(xa, xb), min(xc, xd)), min(min(ya, yb), min(yc, yd)), -depth_far);
    vec3 bounds_maximum = vec3(max(max(xa, xb), max(xc, xd)), max(max(ya, yb), max(yc, yd)), -depth_near);

    vec3 slack = (bounds_maximum - bounds_minimum) * 0.01 + vec3(1.0);

    bounds_minimum -= slack;
    bounds_maximum += slack;

    uint light_base  = range.x;
    uint light_count = range.y;
    uint cluster_base = range.z;
    uint index_limit  = range.w;

    uint local_indices[kMaximumPerCluster];

    uint found = 0u;

    for (uint i = 0u; i < light_count; i++)
    {
        GpuLight light = gpu_lights[light_base + i];

        vec3 centre = light.position_radius.xyz;

        float radius = light.position_radius.w;

        vec3 closest = clamp(centre, bounds_minimum, bounds_maximum);

        vec3 delta = centre - closest;

        if (dot(delta, delta) > radius * radius)
            continue;

        if (found < kMaximumPerCluster)
            local_indices[found++] = light_base + i;
    }

    uint destination = cluster_base + cluster;

    if (found == 0u)
    {
        gpu_clusters[destination] = 0u;
        return;
    }

    uint base = atomicAdd(gpu_light_counter[0], found);

    uint room = (base < index_limit) ? (index_limit - base) : 0u;

    uint keep = min(found, room);

    for (uint i = 0u; i < keep; i++)
        gpu_light_indices[base + i] = local_indices[i];

    gpu_clusters[destination] = (keep > 0u) ? ((base << 8) | keep) : 0u;
}

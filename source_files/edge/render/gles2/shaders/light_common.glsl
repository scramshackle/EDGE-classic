const float kLightTileSize = 16.0;

vec4 LightDataTexel(float index, float row)
{
    return texture2D(u_light_data, vec2((index + 0.5) * u_light_data_step, (row + 0.5) * 0.25));
}

void AccumulateTileLights(vec3 surface_position, vec3 surface_normal, float use_normal, inout vec3 modulate_sum,
                          inout vec3 additive_sum)
{
    float tile_x = floor((gl_FragCoord.x - u_light_view.x) / kLightTileSize);
    float tile_y = floor((gl_FragCoord.y - u_light_view.y) / kLightTileSize);

    if (tile_x < 0.0 || tile_y < 0.0)
        return;

    vec4 header = texture2D(u_light_headers, vec2((tile_x + 0.5) * u_light_view.z, (tile_y + 0.5) * u_light_view.w));

    float count = floor(header.a * 255.0 + 0.5);

    if (count < 0.5)
        return;

    float offset = floor(header.r * 255.0 + 0.5) * 65536.0 + floor(header.g * 255.0 + 0.5) * 256.0 +
                   floor(header.b * 255.0 + 0.5);

    for (int slot = 0; slot < EDGE_LIGHT_MAX_PER_TILE; slot++)
    {
        if (float(slot) >= count)
            break;

        float entry = offset + float(slot);

        float entry_y = floor(entry * u_light_list.y);
        float entry_x = entry - entry_y * u_light_list.x;

        float index = floor(
            texture2D(u_light_indices, vec2((entry_x + 0.5) * u_light_list.y, (entry_y + 0.5) * u_light_list.z)).r *
                255.0 +
            0.5);

        vec4 t0 = LightDataTexel(index, 0.0);
        vec4 t1 = LightDataTexel(index, 1.0);

        vec3 light_position;

        light_position.x = u_light_bounds_min.x + (t0.r + t0.g / 255.0) * u_light_bounds_range.x;
        light_position.y = u_light_bounds_min.y + (t0.b + t0.a / 255.0) * u_light_bounds_range.y;
        light_position.z = u_light_bounds_min.z + (t1.r + t1.g / 255.0) * u_light_bounds_range.z;

        float radius = (t1.b + t1.a / 255.0) * u_light_radius_scale;

        vec3 delta = light_position - surface_position;

        float normalised = dot(delta, delta) / (radius * radius);

        if (normalised >= 1.0)
            continue;

        vec4 t2 = LightDataTexel(index, 2.0);

        float shade = 1.0;

        if (use_normal > 0.5)
        {
            shade = max(0.6 - 0.7 * dot(normalize(-delta), surface_normal), 0.0);
        }

        vec3 contribution = t2.rgb * exp(-5.44 * normalised) * shade;

        if (t2.a > 0.5)
            additive_sum += contribution;
        else
            modulate_sum += contribution;
    }
}

void AccumulateGlows(vec3 surface_position, inout vec3 modulate_sum, inout vec3 additive_sum)
{
    for (int index = 0; index < EDGE_LIGHT_MAX_GLOWS; index++)
    {
        if (float(index) >= u_glow_count)
            break;

        float plane_distance = dot(vec4(surface_position, 1.0), u_glow_plane[index]);

        float radius = u_glow_color[index].a;

        float normalised = (plane_distance * plane_distance) / (radius * radius);

        if (normalised >= 1.0)
            continue;

        vec3 contribution = u_glow_color[index].rgb * exp(-5.44 * normalised);

        if (u_glow_additive[index] > 0.5)
            additive_sum += contribution;
        else
            modulate_sum += contribution;
    }
}

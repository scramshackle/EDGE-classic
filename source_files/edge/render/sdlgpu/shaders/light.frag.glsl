#version 450

layout(set = 3, binding = 0) uniform LightFragmentParameters
{
    vec4  surface_normal;
    vec4  light_position_radius[4];
    vec4  light_color[4];
    int   light_count;
    float normal_horizontal;
    float surface_mode;
    float alpha;
    float alpha_test;
    float light_padding0;
    float light_padding1;
    float light_padding2;
};

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;

layout(location = 0) out vec4 frag_color;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 world_position;

void main()
{
    vec4 surface = texture(tex0, uv);

    vec4 base;

    if (surface_mode < 0.5)
        base = surface;
    else if (surface_mode < 1.5)
        base = vec4(1.0, 1.0, 1.0, 1.0);
    else
        base = vec4(1.0, 1.0, 1.0, surface.a);

    vec3 accumulated = vec3(0.0, 0.0, 0.0);

    for (int index = 0; index < 4; index++)
    {
        if (index >= light_count)
            break;

        vec3  delta  = world_position - light_position_radius[index].xyz;
        float radius = light_position_radius[index].w;

        vec2  light_coordinates;
        float plane_distance;

        if (normal_horizontal > 0.5)
        {
            light_coordinates = vec2((1.0 + delta.x / radius) * 0.5, (1.0 + delta.y / radius) * 0.5);

            plane_distance = abs(delta.z) / radius;
        }
        else
        {
            float scaled_radius = radius / surface_normal.w;

            float tangent = surface_normal.x * delta.y - surface_normal.y * delta.x;

            light_coordinates = vec2((1.0 + tangent / scaled_radius) * 0.5, (1.0 + delta.z / scaled_radius) * 0.5);

            plane_distance = abs(dot(surface_normal.xyz, delta)) / scaled_radius;
        }

        float intensity = exp(-5.44 * plane_distance * plane_distance);

        accumulated += light_color[index].rgb * intensity * texture(tex1, light_coordinates).rgb;
    }

    float output_alpha = base.a * alpha;

    if (output_alpha < alpha_test)
        discard;

    frag_color = vec4(base.rgb * accumulated, output_alpha);
}

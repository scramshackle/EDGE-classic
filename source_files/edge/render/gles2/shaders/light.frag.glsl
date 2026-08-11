uniform sampler2D u_surface_texture;
uniform sampler2D u_light_texture;

uniform float u_surface_mode;
uniform float u_alpha;
uniform float u_alpha_test;

uniform vec4 u_surface_normal;
uniform float u_normal_horizontal;

uniform int  u_light_count;
uniform vec4 u_light_position_radius[4];
uniform vec4 u_light_color[4];

varying vec2 v_texture_coordinates;
varying vec3 v_world_position;

void main()
{
    vec4 surface = texture2D(u_surface_texture, v_texture_coordinates);

    vec4 base;

    if (u_surface_mode < 0.5)
        base = surface;
    else if (u_surface_mode < 1.5)
        base = vec4(1.0, 1.0, 1.0, 1.0);
    else
        base = vec4(1.0, 1.0, 1.0, surface.a);

    vec3 accumulated = vec3(0.0, 0.0, 0.0);

    for (int index = 0; index < 4; index++)
    {
        if (index >= u_light_count)
            break;

        vec3  delta  = v_world_position - u_light_position_radius[index].xyz;
        float radius = u_light_position_radius[index].w;

        vec2  light_coordinates;
        float plane_distance;

        if (u_normal_horizontal > 0.5)
        {
            light_coordinates = vec2((1.0 + delta.x / radius) * 0.5, (1.0 + delta.y / radius) * 0.5);

            plane_distance = abs(delta.z) / radius;
        }
        else
        {
            float scaled_radius = radius / u_surface_normal.w;

            float tangent = u_surface_normal.x * delta.y - u_surface_normal.y * delta.x;

            light_coordinates =
                vec2((1.0 + tangent / scaled_radius) * 0.5, (1.0 + delta.z / scaled_radius) * 0.5);

            plane_distance = abs(dot(u_surface_normal.xyz, delta)) / scaled_radius;
        }

        float intensity = exp(-5.44 * plane_distance * plane_distance);

        accumulated += u_light_color[index].rgb * intensity * texture2D(u_light_texture, light_coordinates).rgb;
    }

    float output_alpha = base.a * u_alpha;

    if (output_alpha < u_alpha_test)
        discard;

    gl_FragColor = vec4(base.rgb * accumulated, output_alpha);
}

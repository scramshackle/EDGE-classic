const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;

uniform sampler2D u_light_data;
uniform sampler2D u_light_headers;
uniform sampler2D u_light_indices;

uniform float u_world_lit;
uniform vec4  u_light_view;
uniform vec4  u_light_list;
uniform vec3  u_light_bounds_min;
uniform vec3  u_light_bounds_range;
uniform float u_light_radius_scale;
uniform float u_light_data_step;

uniform float u_glow_count;
uniform vec4  u_glow_plane[EDGE_LIGHT_MAX_GLOWS];
uniform vec4  u_glow_color[EDGE_LIGHT_MAX_GLOWS];
uniform vec4  u_glow_additive;


uniform float u_alpha;
uniform float u_alpha_test;
uniform float u_additive_pass;

uniform float u_fog_mode;
uniform vec4  u_fog_color;
uniform float u_fog_density;
uniform float u_fog_start;
uniform float u_fog_end;

uniform float u_oit_mode;
uniform float u_oit_scale;

varying vec4 v_color_and_u;
varying vec4 v_eye_and_v;
varying vec3 v_normal;

EDGE_INCLUDE_LIGHT_COMMON

float FogFactor()
{
    if (u_fog_mode <= 0.5)
    {
        return 0.0;
    }

    float fog_distance = length(v_eye_and_v.xyz);

    if (u_fog_mode < kFogLinear + 0.5)
    {
        return clamp(smoothstep(u_fog_start, u_fog_end, fog_distance), 0.0, 1.0);
    }

    return 1.0 - clamp(exp2(-u_fog_density * u_fog_density * fog_distance * fog_distance * kLog2), 0.0, 1.0);
}

float OitWeight(float alpha, float view_depth)
{
    float a = min(1.0, alpha * 10.0) + 0.01;
    float d = 1.0 - view_depth * 0.9;

    return clamp(a * a * a * 1e8 * d * d * d, 1e-2, 3e3);
}

void main()
{
    vec4 texel = texture2D(u_texture0, vec2(v_color_and_u.w, v_eye_and_v.w));

    if (u_alpha_test > 0.0 && texel.a < u_alpha_test)
    {
        discard;
    }

    vec3 modulate_sum = vec3(0.0, 0.0, 0.0);
    vec3 additive_sum = vec3(0.0, 0.0, 0.0);

    if (u_world_lit > 0.5)
    {
        AccumulateTileLights(v_eye_and_v.xyz, normalize(v_normal), 1.0, modulate_sum, additive_sum);
    }

    if (u_glow_count > 0.5)
    {
        AccumulateGlows(v_eye_and_v.xyz, modulate_sum, additive_sum);
    }

    vec3 rgb = mix(texel.rgb * v_color_and_u.rgb, v_color_and_u.rgb, u_additive_pass);

    float fog_factor = FogFactor();

    if (fog_factor > 0.0)
    {
        rgb = mix(rgb, u_fog_color.rgb, fog_factor);
    }

    rgb += (texel.rgb * modulate_sum + additive_sum) * (1.0 - u_additive_pass);

    vec4 fragment_color = vec4(rgb, texel.a * u_alpha);

    if (u_oit_mode > 0.5)
    {
        float view_depth = clamp(-v_eye_and_v.z * 0.000625, 0.0, 1.0);

        float weight = OitWeight(fragment_color.a, view_depth) * u_oit_scale;

        if (u_oit_mode > 1.5)
        {
            gl_FragColor = vec4(fragment_color.a, 0.0, 0.0, fragment_color.a);
            return;
        }

        gl_FragColor = vec4(fragment_color.rgb * fragment_color.a, fragment_color.a) * weight;
        return;
    }

    gl_FragColor = fragment_color;
}

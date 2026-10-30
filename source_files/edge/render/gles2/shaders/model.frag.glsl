const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;
uniform sampler2D u_color_lookup;
uniform float     u_color_lookup_enabled;

uniform sampler2D u_light_data;
uniform sampler2D u_light_headers;
uniform sampler2D u_light_indices;

uniform float u_world_lit;
uniform vec4  u_light_view;
uniform vec4  u_light_cluster;
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

uniform float u_fog_mode;
uniform vec4  u_fog_color;
uniform float u_fog_density;
uniform float u_fog_start;
uniform float u_fog_end;

uniform float u_oit_mode;
uniform float u_oit_scale;
uniform sampler2D u_oit_front;
uniform vec2      u_oit_front_scale;

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

const float kOitAccumulate     = 1.0;
const float kOitRevealage      = 2.0;
const float kOitFrontDepth     = 5.0;
const float kOitFrontColor     = 6.0;
const float kOitFrontClear     = 65504.0;
const float kOitFrontTolerance = 1.0 + 1.0 / 512.0;

bool OitModeIs(float mode)
{
    return abs(u_oit_mode - mode) < 0.5;
}

bool OitFrontFragment(float view_distance)
{
    float front = texture2D(u_oit_front, gl_FragCoord.xy * u_oit_front_scale).r;

    return view_distance <= front * kOitFrontTolerance;
}

float OitWeight(float alpha, float view_distance)
{
    if (u_oit_scale < 1.0)
        return 3e3 * u_oit_scale;

    float d = 32.0 / max(view_distance, 32.0);

    return alpha * clamp(3e3 * d * d * d, 1e-2, 3e3);
}

vec3 ApplyColorLookup(vec3 source)
{
    vec3  scaled = clamp(source, 0.0, 1.0) * 63.0;
    float blue0  = floor(scaled.b);
    float blue1  = min(blue0 + 1.0, 63.0);
    vec2  inner  = (scaled.rg + 0.5) / 512.0;
    vec2  tile0  = vec2(mod(blue0, 8.0), floor(blue0 / 8.0)) * 0.125;
    vec2  tile1  = vec2(mod(blue1, 8.0), floor(blue1 / 8.0)) * 0.125;
    vec3  low    = texture2D(u_color_lookup, tile0 + inner).rgb;
    vec3  high   = texture2D(u_color_lookup, tile1 + inner).rgb;

    return mix(low, high, scaled.b - blue0);
}

void main()
{
    vec4 texel = texture2D(u_texture0, vec2(v_color_and_u.w, v_eye_and_v.w));

    if (u_alpha_test > 0.0 && texel.a < u_alpha_test)
    {
        discard;
    }

    if (u_oit_mode > 0.5)
    {
        float view_distance = -v_eye_and_v.z;

        if (OitModeIs(kOitFrontDepth))
        {
            gl_FragColor = vec4(texel.a * u_alpha < 1.0 / 255.0 ? kOitFrontClear : view_distance);
            return;
        }

        if (u_oit_front_scale.x > 0.0 && OitFrontFragment(view_distance) != OitModeIs(kOitFrontColor))
        {
            gl_FragColor = vec4(0.0);
            return;
        }

        if (OitModeIs(kOitRevealage))
        {
            gl_FragColor = vec4(texel.a * u_alpha, 0.0, 0.0, texel.a * u_alpha);
            return;
        }
    }

    if (u_color_lookup_enabled > 0.5)
    {
        texel.rgb = ApplyColorLookup(texel.rgb);
    }

    vec3 modulate_sum = vec3(0.0, 0.0, 0.0);
    vec3 additive_sum = vec3(0.0, 0.0, 0.0);

    if (u_world_lit > 0.5)
    {
        AccumulateClusterLights(v_eye_and_v.xyz, normalize(v_normal), 1.0, modulate_sum, additive_sum);
    }

    if (u_glow_count > 0.5)
    {
        AccumulateGlows(v_eye_and_v.xyz, modulate_sum, additive_sum);
    }

    vec3 rgb = texel.rgb * v_color_and_u.rgb;

    float fog_factor = FogFactor();

    if (fog_factor > 0.0)
    {
        rgb = mix(rgb, u_fog_color.rgb, fog_factor);
    }

    rgb += texel.rgb * modulate_sum + additive_sum;

    vec4 fragment_color = vec4(rgb, texel.a * u_alpha);

    if (OitModeIs(kOitAccumulate))
    {
        float weight = OitWeight(fragment_color.a, -v_eye_and_v.z);

        gl_FragColor = vec4(fragment_color.rgb * fragment_color.a, fragment_color.a) * weight;
        return;
    }

    gl_FragColor = fragment_color;
}

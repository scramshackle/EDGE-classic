const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;
uniform sampler2D u_texture1;


uniform float u_multi_texture;
uniform float u_line_mode;
uniform float u_skip_rgb;
uniform float u_alpha_test;

uniform float u_fog_mode;
uniform vec4  u_fog_color;
uniform float u_fog_density;
uniform float u_fog_start;
uniform float u_fog_end;

uniform float u_sky_pass;
uniform mat4  u_sky_inverse_projection;
uniform mat4  u_sky_inverse_view;
uniform vec4  u_sky_viewport;
uniform float u_sky_stretch_mode;
uniform float u_sky_u_scale;
uniform float u_sky_ty;
uniform float u_sky_u_offset;
uniform float u_sky_v_offset;
uniform float u_sky_vertical_fov_slope;
uniform float u_sky_horizon_shift;
uniform float u_sky_is_box;

uniform float u_oit_mode;
uniform float u_oit_scale;

uniform samplerCube u_sky_cube;

varying vec4 v_texture_coordinates;
varying vec4 v_color;
varying vec3 v_eye_position;

const float kSkyStretchMirror  = 0.0;
const float kSkyStretchRepeat  = 1.0;
const float kSkyStretchStretch = 2.0;
const float kSkyStretchVanilla = 3.0;

const int   kSkyPinchTaps      = 64;
const float kSkyPinchTapWeight = 0.015625;
const float kSkyPinchFadeStart = 0.9397;
const float kSkyPinchFadeEnd   = 0.9848;

float FogFactor()
{
    if (u_fog_mode <= 0.5)
    {
        return 0.0;
    }

    float fog_distance = length(v_eye_position);

    if (u_fog_mode < kFogLinear + 0.5)
    {
        return clamp(smoothstep(u_fog_start, u_fog_end, fog_distance), 0.0, 1.0);
    }

    return 1.0 - clamp(exp2(-u_fog_density * u_fog_density * fog_distance * fog_distance * kLog2), 0.0, 1.0);
}

vec3 SkyDirection()
{
    vec2 ndc  = ((gl_FragCoord.xy - u_sky_viewport.xy) / u_sky_viewport.zw) * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);
    vec4 eye  = u_sky_inverse_projection * clip;
    eye       = vec4(eye.xy, -1.0, 0.0);
    return normalize((u_sky_inverse_view * eye).xyz);
}

vec4 SampleCubeSky()
{
    vec3 dir = SkyDirection();

    vec4 sampled = textureCube(u_sky_cube, vec3(dir.x, dir.z, -dir.y));

    vec3 rgb = sampled.rgb * v_color.rgb;

    float fog_factor = FogFactor();

    if (fog_factor > 0.0)
    {
        rgb = mix(rgb, u_fog_color.rgb, fog_factor);
    }

    return vec4(rgb, sampled.a * v_color.a);
}

vec4 SampleEquirectSky()
{
    vec3 dir = SkyDirection();

    const float kTwoPi = 6.28318530718;

    float horiz_len = max(length(dir.xy), 0.0001);
    float sky_tan   = dir.z / horiz_len - u_sky_horizon_shift;
    float p         = sky_tan / sqrt(1.0 + sky_tan * sky_tan);
    float u = fract((atan(dir.y, dir.x) / kTwoPi + 0.5) * u_sky_u_scale + u_sky_u_offset);

    bool mode_is_vanilla = abs(u_sky_stretch_mode - kSkyStretchVanilla) < 0.5;
    bool mode_is_mirror  = abs(u_sky_stretch_mode - kSkyStretchMirror) < 0.5;
    bool lower_hemisphere = p < 0.0;

    float v_raw;

    if (mode_is_vanilla)
    {
        float pc = clamp(sky_tan / u_sky_vertical_fov_slope, -1.0, 1.0);
        v_raw           = (pc + 1.0) * 0.5 * u_sky_ty - u_sky_v_offset;
    }
    else if (mode_is_mirror)
    {
        float base = (p + 1.0) * 0.5 * u_sky_ty;
        v_raw      = lower_hemisphere ? -(base + u_sky_v_offset) : (base - u_sky_v_offset);
    }
    else
    {
        v_raw = (p + 1.0) * 0.5 * u_sky_ty - u_sky_v_offset;
    }

    float v = fract(v_raw);

    vec4 sampled = texture2D(u_texture0, vec2(u, v));

    float pinch_mix = smoothstep(kSkyPinchFadeStart, kSkyPinchFadeEnd, abs(dir.z));

    if (pinch_mix > 0.0)
    {
        vec4 averaged = vec4(0.0);

        float average_span = min(u_sky_u_scale, 1.0);

        for (int i = 0; i < kSkyPinchTaps; i++)
        {
            float tap = (float(i) + 0.5) * kSkyPinchTapWeight - 0.5;

            averaged += texture2D(u_texture0, vec2(fract(u + tap * average_span), v));
        }

        sampled = mix(sampled, averaged * kSkyPinchTapWeight, pinch_mix);
    }

    vec3 rgb = sampled.rgb * v_color.rgb;

    float fog_factor = FogFactor();

    if (fog_factor > 0.0)
    {
        rgb = mix(rgb, u_fog_color.rgb, fog_factor);
    }

    return vec4(rgb, sampled.a * v_color.a);
}

float OitWeight(float alpha, float view_depth)
{
    float a = min(1.0, alpha * 10.0) + 0.01;
    float d = 1.0 - view_depth * 0.9;

    return clamp(a * a * a * 1e8 * d * d * d, 1e-2, 3e3);
}

void main()
{
    if (u_sky_pass > 0.5)
    {
        if (u_sky_is_box > 0.5)
            gl_FragColor = SampleCubeSky();
        else
            gl_FragColor = SampleEquirectSky();
        return;
    }

    if (u_line_mode > 0.5)
    {
        float u           = v_texture_coordinates.x;
        float v           = v_texture_coordinates.y;
        float line_width  = v_texture_coordinates.z;
        float line_length = v_texture_coordinates.w;
        float aa_radius   = 1.0;

        float au = 1.0 - smoothstep(1.0 - ((3.0 * aa_radius) / line_width), 1.0, abs(u / line_width));
        float av = 1.0 - smoothstep(1.0 - ((3.0 * aa_radius) / line_length), 1.0, abs(v / line_length));

        gl_FragColor = vec4(v_color.rgb, v_color.a * min(au, av));
        return;
    }

    vec4 texel0 = texture2D(u_texture0, v_texture_coordinates.xy);

    if (u_alpha_test > 0.0 && texel0.a < u_alpha_test)
    {
        discard;
    }

    texel0 = mix(texel0, vec4(1.0, 1.0, 1.0, texel0.a), u_skip_rgb);

    vec4 fragment_color = v_color;

    float fog_factor = FogFactor();

    if (u_multi_texture > 0.5)
    {
        fragment_color *= texel0;
        fragment_color *= texture2D(u_texture1, v_texture_coordinates.zw);

        if (fog_factor > 0.0)
        {
            fragment_color = mix(fragment_color, u_fog_color, fog_factor);
        }
    }
    else
    {
        fragment_color *= texel0;

        if (fog_factor > 0.0)
        {
            fragment_color.rgb = mix(fragment_color, u_fog_color, fog_factor).rgb;
        }
    }

    if (u_oit_mode > 0.5)
    {
        float view_depth = clamp(-v_eye_position.z * 0.000625, 0.0, 1.0);

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

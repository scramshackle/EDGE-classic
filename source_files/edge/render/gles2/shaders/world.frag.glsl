const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform sampler2D u_color_lookup;
uniform float     u_color_lookup_enabled;
uniform float     u_whiten;
uniform vec4      u_blur;
uniform vec4      u_liquid;


uniform float u_multi_texture;
uniform float u_light_falloff;

uniform sampler2D u_light_data;
uniform sampler2D u_light_headers;
uniform sampler2D u_light_indices;

uniform float u_world_lit;
uniform vec4  u_light_view;
uniform vec4  u_light_cluster;
uniform vec4  u_light_list;

uniform float u_glow_count;
uniform vec4  u_glow_plane[EDGE_LIGHT_MAX_GLOWS];
uniform vec4  u_glow_color[EDGE_LIGHT_MAX_GLOWS];
uniform vec4  u_glow_additive;
uniform vec3  u_light_bounds_min;
uniform vec3  u_light_bounds_range;
uniform float u_light_radius_scale;
uniform float u_light_data_step;
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

    vec4 sampled = textureCube(u_sky_cube, vec3(dir.y, -dir.z, dir.x));

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

        float average_span = min(abs(u_sky_u_scale), 1.0);

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

EDGE_INCLUDE_LIGHT_COMMON

float OitWeight(float alpha, float view_depth)
{
    float a = min(1.0, alpha * 10.0) + 0.01;
    float d = 1.0 - view_depth * 0.9;

    return clamp(a * a * a * 1e8 * d * d * d, 1e-2, 3e3);
}

vec2 LiquidFlow(vec2 point, vec2 direction, float time, float seed)
{
    float k = 6.2831853 / 72.0;
    vec2  q = point + direction * 6.0 * time;

    q += 4.0 * vec2(sin(q.y * k + time * 0.9 + seed), sin(q.x * k * 1.13 - time * 0.7 + seed * 1.7));
    q += 2.4 * vec2(sin((q.x + q.y) * k * 0.61 + time * 0.53 + seed * 2.3),
                    sin((q.x - q.y) * k * 0.79 - time * 0.61 + seed * 0.4));

    return q;
}

vec4 SampleLiquid(vec2 coordinate)
{
    vec2  size  = u_liquid.zw;
    vec2  point = coordinate * size;
    float time  = u_liquid.y;

    if (u_liquid.x < 1.5)
    {
        float k     = 6.2831853 / 40.0;
        float swirl = time * 2.0;

        point += vec2(sin(point.y * k + swirl * 1.7) + sin(point.x * k * 0.5 + swirl * 1.1 + 1.3),
                      sin(point.x * k + swirl * 1.3 + 0.7) + sin(point.y * k * 0.5 - swirl * 0.9 + 2.1));

        float kt = 6.2831853 / 320.0;

        point += 3.0 * vec2(sin((point.x + point.y) * kt + swirl * 0.8),
                            sin((point.y - point.x) * kt * 1.21 + swirl * 0.6));

        return texture2D(u_texture0, point / size);
    }

    if (u_liquid.x < 2.5)
    {
        vec4 lower = texture2D(u_texture0, LiquidFlow(point, vec2(0.8, 0.6), time, 0.0) / size);
        vec4 upper = texture2D(u_texture0, LiquidFlow(point + 25.0, vec2(-0.97386, 0.22717), time * 1.3, 3.1) / size);

        return mix(lower, upper, 0.33);
    }

    float row = floor(point.y) + 0.5;

    point.x += 2.0 * sin(row * 6.2831853 / 16.0 + time * 4.0);

    return texture2D(u_texture0, point / size);
}

vec4 SampleBlurred(vec2 coordinate)
{
    vec2  size  = u_blur.zw;
    vec2  point = coordinate * size - 0.5;
    vec2  base  = floor(point);
    float scale = -0.5 / (u_blur.x * u_blur.x);
    vec4  sum   = vec4(0.0);
    float total = 0.0;

    for (int j = -1; j <= 2; j++)
    {
        for (int i = -1; i <= 2; i++)
        {
            vec2  center = base + vec2(float(i), float(j));
            vec2  delta  = center - point;
            float weight = exp(dot(delta, delta) * scale);

            sum += texture2D(u_texture0, (center + 0.5) / size) * weight;
            total += weight;
        }
    }

    return sum / total;
}

vec3 WhitenColor(vec3 source)
{
    float brightest = max(source.r, max(source.g, source.b));

    return vec3((brightest * 196.0 + (source.r + source.g + source.b) * 20.0) / 256.0);
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
    if (u_sky_pass > 0.5)
    {
        if (u_sky_is_box > 0.5)
            gl_FragColor = SampleCubeSky();
        else
            gl_FragColor = SampleEquirectSky();

        if (u_color_lookup_enabled > 0.5)
            gl_FragColor.rgb = ApplyColorLookup(gl_FragColor.rgb);
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

    vec4 texel0;

    if (u_blur.x > 0.0)
        texel0 = SampleBlurred(v_texture_coordinates.xy);
    else if (u_liquid.x > 0.5)
        texel0 = SampleLiquid(v_texture_coordinates.xy);
    else
        texel0 = texture2D(u_texture0, v_texture_coordinates.xy);


    if (u_alpha_test > 0.0 && texel0.a < u_alpha_test)
    {
        discard;
    }

    if (u_color_lookup_enabled > 0.5)
    {
        texel0.rgb = ApplyColorLookup(texel0.rgb);
    }

    if (u_whiten > 0.5)
    {
        texel0.rgb = WhitenColor(texel0.rgb);
    }

    texel0 = mix(texel0, vec4(1.0, 1.0, 1.0, texel0.a), u_skip_rgb);

    vec4 fragment_color = v_color;

    float fog_factor = FogFactor();

    vec3 modulate_sum = vec3(0.0, 0.0, 0.0);
    vec3 additive_sum = vec3(0.0, 0.0, 0.0);

    if (u_world_lit > 0.5)
    {
        AccumulateClusterLights(v_eye_position, vec3(0.0, 0.0, 1.0), 0.0, modulate_sum, additive_sum);
    }

    if (u_glow_count > 0.5)
    {
        AccumulateGlows(v_eye_position, modulate_sum, additive_sum);
    }

    if (u_multi_texture > 0.5 || u_light_falloff > 0.5)
    {
        fragment_color *= texel0;

        if (u_light_falloff > 0.5)
        {
            vec2 falloff = v_texture_coordinates.zw * 2.0 - 1.0;

            fragment_color.rgb *= exp(-5.44 * dot(falloff, falloff));
        }
        else
        {
            fragment_color *= texture2D(u_texture1, v_texture_coordinates.zw);
        }

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

    fragment_color.rgb += texel0.rgb * modulate_sum + additive_sum;

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

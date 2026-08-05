const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;

uniform vec4 u_clip_plane[6];

uniform float u_alpha;
uniform float u_alpha_test;
uniform float u_additive_pass;

uniform float u_fog_mode;
uniform vec4  u_fog_color;
uniform float u_fog_density;
uniform float u_fog_start;
uniform float u_fog_end;

varying vec2 v_texture_coordinates;
varying vec3 v_color;
varying vec3 v_eye_position;

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

void main()
{
    vec4 eye_position = vec4(v_eye_position, 1.0);

    float clip_distance = 0.0;

    for (int i = 0; i < 6; i++)
    {
        clip_distance += min(0.0, dot(eye_position, u_clip_plane[i]));
    }

    if (clip_distance < 0.0)
    {
        discard;
    }

    vec4 texel = texture2D(u_texture0, v_texture_coordinates);

    if (u_alpha_test > 0.0 && texel.a < u_alpha_test)
    {
        discard;
    }

    vec3 rgb = mix(texel.rgb * v_color, v_color, u_additive_pass);

    float fog_factor = FogFactor();

    if (fog_factor > 0.0)
    {
        rgb = mix(rgb, u_fog_color.rgb, fog_factor);
    }

    gl_FragColor = vec4(rgb, texel.a * u_alpha);
}

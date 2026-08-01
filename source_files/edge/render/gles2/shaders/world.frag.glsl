const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;
uniform sampler2D u_texture1;

uniform vec4 u_clip_plane[6];

uniform float u_multi_texture;
uniform float u_line_mode;
uniform float u_skip_rgb;
uniform float u_alpha_test;

uniform float u_fog_mode;
uniform vec4  u_fog_color;
uniform float u_fog_density;
uniform float u_fog_start;
uniform float u_fog_end;

varying vec4 v_texture_coordinates;
varying vec4 v_color;
varying vec3 v_eye_position;

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

    float fog_factor = 0.0;

    if (u_fog_mode > 0.5)
    {
        float fog_distance = length(v_eye_position);

        if (u_fog_mode < kFogLinear + 0.5)
        {
            fog_factor = clamp(smoothstep(u_fog_start, u_fog_end, fog_distance), 0.0, 1.0);
        }
        else
        {
            fog_factor =
                1.0 - clamp(exp2(-u_fog_density * u_fog_density * fog_distance * fog_distance * kLog2), 0.0, 1.0);
        }
    }

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

    gl_FragColor = fragment_color;
}

const float kFogLinear = 1.0;
const float kLog2      = 1.442695;

uniform sampler2D u_texture0;


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

float OitWeight(float alpha, float view_depth)
{
    float a = min(1.0, alpha * 10.0) + 0.01;
    float d = 1.0 - view_depth * 0.9;

    return clamp(a * a * a * 1e8 * d * d * d, 1e-2, 3e3);
}

void main()
{
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

    vec4 fragment_color = vec4(rgb, texel.a * u_alpha);

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

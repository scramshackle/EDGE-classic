uniform mat4 u_model_view_projection;
uniform mat4 u_model_view;

uniform float u_sky_pass;
uniform float u_sky_fog_depth;
uniform float u_sky_geometry;

uniform float u_light_depth;
uniform vec4  u_view_tint;

attribute vec3 a_position;
attribute vec4 a_texture_coordinates;
attribute vec4 a_color;

varying vec4 v_texture_coordinates;
varying vec4 v_color;
varying vec3 v_eye_position;

void main()
{
    vec4 model_position = vec4(a_position, 1.0);

    v_texture_coordinates = a_texture_coordinates;
    v_color               = a_color;
    v_color.rgb *= u_view_tint.rgb;

    if (u_sky_pass > 0.5)
    {
        v_eye_position = vec3(0.0, 0.0, -u_sky_fog_depth);

        if (u_sky_geometry > 0.5)
            gl_Position = u_model_view_projection * model_position;
        else
            gl_Position = vec4(a_position.xy, 1.0, 1.0);
    }
    else
    {
        v_eye_position = (u_model_view * model_position).xyz;

        if (u_light_depth > 0.5)
        {
            v_texture_coordinates.z = -v_eye_position.z * 0.000625;
        }

        gl_Position = u_model_view_projection * model_position;
    }
}

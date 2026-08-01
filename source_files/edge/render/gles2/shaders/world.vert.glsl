uniform mat4 u_model_view_projection;
uniform mat4 u_model_view;

attribute vec3 a_position;
attribute vec4 a_texture_coordinates;
attribute vec4 a_color;

varying vec4 v_texture_coordinates;
varying vec4 v_color;
varying vec3 v_eye_position;

void main()
{
    vec4 model_position = vec4(a_position, 1.0);

    gl_Position = u_model_view_projection * model_position;

    v_texture_coordinates = a_texture_coordinates;
    v_color               = a_color;
    v_eye_position        = (u_model_view * model_position).xyz;
}

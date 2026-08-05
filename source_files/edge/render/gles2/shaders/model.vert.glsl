uniform mat4 u_model_view_projection;
uniform mat4 u_model_view;
uniform mat4 u_model_transform;

uniform float u_lerp;

uniform vec2 u_texture_scale;
uniform vec2 u_texture_offset;

attribute vec3 a_position_frame1;
attribute vec3 a_position_frame2;
attribute vec2 a_texture_coordinates;
attribute vec3 a_color;

varying vec2 v_texture_coordinates;
varying vec3 v_color;
varying vec3 v_eye_position;

void main()
{
    vec3 blended = mix(a_position_frame1, a_position_frame2, u_lerp);

    vec4 model_position = u_model_transform * vec4(blended, 1.0);

    v_texture_coordinates = a_texture_coordinates * u_texture_scale + u_texture_offset;
    v_color               = a_color;
    v_eye_position        = (u_model_view * model_position).xyz;

    gl_Position = u_model_view_projection * model_position;
}

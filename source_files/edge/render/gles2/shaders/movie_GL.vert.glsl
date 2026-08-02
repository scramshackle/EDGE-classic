#version 110
attribute vec3 a_position;
attribute vec4 a_texture_coordinates;

uniform mat4 u_model_view;
uniform mat4 u_projection;

varying vec2 v_texture_coordinates;

void main()
{
    v_texture_coordinates = a_texture_coordinates.xy;

    gl_Position = u_projection * u_model_view * vec4(a_position, 1.0);
}

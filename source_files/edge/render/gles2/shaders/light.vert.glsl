uniform mat4 u_model_view_projection;

attribute vec3 a_position;
attribute vec4 a_texture_coordinates;

varying vec2 v_texture_coordinates;
varying vec3 v_world_position;

void main()
{
    v_texture_coordinates = a_texture_coordinates.xy;
    v_world_position      = a_position;

    gl_Position = u_model_view_projection * vec4(a_position, 1.0);
}

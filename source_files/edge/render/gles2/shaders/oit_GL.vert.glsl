#version 110
#define EDGE_LIGHT_MAX_PER_TILE 64
#define EDGE_LIGHT_MAX_GLOWS 2
attribute vec3 a_position;
attribute vec4 a_texture_coordinates;

varying vec2 v_texture_coordinates;

void main()
{
    v_texture_coordinates = a_texture_coordinates.xy;

    gl_Position = vec4(a_position, 1.0);
}

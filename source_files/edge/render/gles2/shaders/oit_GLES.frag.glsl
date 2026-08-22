#version 100
#define EDGE_LIGHT_MAX_PER_TILE 64
#define EDGE_LIGHT_MAX_GLOWS 2
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_accumulation;
uniform sampler2D u_revealage;

uniform float u_oit_scale;

varying vec2 v_texture_coordinates;

void main()
{
    vec4 accumulation = texture2D(u_accumulation, v_texture_coordinates);

    float revealage = texture2D(u_revealage, v_texture_coordinates).a;

    accumulation /= u_oit_scale;

    float weight = max(accumulation.a, 1e-5);

    gl_FragColor = vec4(accumulation.rgb / weight, 1.0 - revealage);
}

#version 110
uniform sampler2D u_texture_y;
uniform sampler2D u_texture_cb;
uniform sampler2D u_texture_cr;

uniform vec2 u_luma_scale;
uniform vec2 u_chroma_scale;

varying vec2 v_texture_coordinates;

const mat4 kRec601 = mat4(1.16438,  0.00000,  1.59603, -0.87079,
                          1.16438, -0.39176, -0.81297,  0.52959,
                          1.16438,  2.01723,  0.00000, -1.08139,
                          0.00000,  0.00000,  0.00000,  1.00000);

void main()
{
    float y  = texture2D(u_texture_y, v_texture_coordinates * u_luma_scale).r;
    float cb = texture2D(u_texture_cb, v_texture_coordinates * u_chroma_scale).r;
    float cr = texture2D(u_texture_cr, v_texture_coordinates * u_chroma_scale).r;

    gl_FragColor = vec4(y, cb, cr, 1.0) * kRec601;
}

#version 450

layout(set = 2, binding = 0) uniform sampler2D texture_y;
layout(set = 2, binding = 1) uniform sampler2D texture_cb;
layout(set = 2, binding = 2) uniform sampler2D texture_cr;

layout(set = 3, binding = 0) uniform MovieFragmentParameters
{
    vec4 plane_scales;
};

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

const mat4 kRec601 = mat4(1.16438,  0.00000,  1.59603, -0.87079,
                          1.16438, -0.39176, -0.81297,  0.52959,
                          1.16438,  2.01723,  0.00000, -1.08139,
                          0.00000,  0.00000,  0.00000,  1.00000);

void main()
{
    float y  = texture(texture_y, uv * plane_scales.xy).r;
    float cb = texture(texture_cb, uv * plane_scales.zw).r;
    float cr = texture(texture_cr, uv * plane_scales.zw).r;

    frag_color = vec4(y, cb, cr, 1.0) * kRec601;
}

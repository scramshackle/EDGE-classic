#version 450

layout(set = 1, binding = 0) uniform ModelVertexParameters
{
    mat4  mvp;
    mat4  mv;
    mat4  model_transform;
    float lerp;
    float vertex_padding0;
    vec2  texture_scale;
    vec2  texture_offset;
    vec2  vertex_padding1;
};

layout(location = 0) in vec3 position_frame1;
layout(location = 1) in vec3 position_frame2;
layout(location = 2) in vec2 texcoords;
layout(location = 3) in vec3 color0;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 color;
layout(location = 2) out vec3 vpos;

void main()
{
    vec3 blended = mix(position_frame1, position_frame2, lerp);

    vec4 model_position = model_transform * vec4(blended, 1.0);
    vec4 vertex         = mv * model_position;

    uv    = texcoords * texture_scale + texture_offset;
    color = color0;

    gl_Position = mvp * model_position;


    vpos = vertex.xyz;
}

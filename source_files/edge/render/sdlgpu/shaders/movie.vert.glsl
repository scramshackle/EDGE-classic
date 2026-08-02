#version 450

layout(set = 1, binding = 0) uniform MovieVertexParameters
{
    mat4 mvp;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 texcoords;
layout(location = 2) in vec4 color0;

layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = mvp * vec4(position, 1.0);

    uv = texcoords.xy;
}

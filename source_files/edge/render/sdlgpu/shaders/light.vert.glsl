#version 450

layout(set = 1, binding = 0) uniform LightVertexParameters
{
    mat4 mvp;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 texcoords;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 world_position;

void main()
{
    uv             = texcoords.xy;
    world_position = position;

    gl_Position = mvp * vec4(position, 1.0);
}

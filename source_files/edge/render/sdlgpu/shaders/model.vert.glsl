#version 450

layout(set = 1, binding = 0) uniform ModelVertexParameters
{
    mat4  mvp;
    mat4  mv;
    mat4  model_transform;
    vec4  clipplane[6];
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
layout(location = 3) out float clipvertex0;
layout(location = 4) out float clipvertex1;
layout(location = 5) out float clipvertex2;
layout(location = 6) out float clipvertex3;
layout(location = 7) out float clipvertex4;
layout(location = 8) out float clipvertex5;

void main()
{
    vec3 blended = mix(position_frame1, position_frame2, lerp);

    vec4 model_position = model_transform * vec4(blended, 1.0);
    vec4 vertex         = mv * model_position;

    uv    = texcoords * texture_scale + texture_offset;
    color = color0;

    gl_Position = mvp * model_position;

    clipvertex0 = dot(vertex, clipplane[0]);
    clipvertex1 = dot(vertex, clipplane[1]);
    clipvertex2 = dot(vertex, clipplane[2]);
    clipvertex3 = dot(vertex, clipplane[3]);
    clipvertex4 = dot(vertex, clipplane[4]);
    clipvertex5 = dot(vertex, clipplane[5]);

    vpos = vertex.xyz;
}

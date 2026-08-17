#version 450

layout(set = 1, binding = 0) uniform VertexParameters
{
    mat4 mvp;
    mat4 tm;
    mat4 mv;
    vec4 clipplane[6];
    float sky_pass;
    float sky_fog_depth;
    float light_depth;
    float vertex_padding1;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 texcoords;
layout(location = 2) in vec4 color0;

layout(location = 0) out vec4 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out vec3 vpos;
layout(location = 3) out float clipvertex0;
layout(location = 4) out float clipvertex1;
layout(location = 5) out float clipvertex2;
layout(location = 6) out float clipvertex3;
layout(location = 7) out float clipvertex4;
layout(location = 8) out float clipvertex5;

void main()
{
    vec4 model_position = vec4(position, 1.0);
    vec4 vertex         = mv * model_position;

    uv    = texcoords;
    color = color0;

    if (sky_pass > 0.5)
    {
        vertex = vec4(0.0, 0.0, -sky_fog_depth, 1.0);

        gl_Position = vec4(position.xy, 1.0, 1.0);
    }
    else
    {
        if (light_depth > 0.5)
        {
            uv.z = -vertex.z * 0.000625;
        }

        gl_Position = mvp * model_position;
    }

    clipvertex0 = dot(vertex, clipplane[0]);
    clipvertex1 = dot(vertex, clipplane[1]);
    clipvertex2 = dot(vertex, clipplane[2]);
    clipvertex3 = dot(vertex, clipplane[3]);
    clipvertex4 = dot(vertex, clipplane[4]);
    clipvertex5 = dot(vertex, clipplane[5]);

    vpos = vertex.xyz;
}

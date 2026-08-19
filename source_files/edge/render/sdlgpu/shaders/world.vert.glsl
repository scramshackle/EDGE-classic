#version 450

layout(set = 1, binding = 0) uniform VertexParameters
{
    mat4 mvp;
    mat4 tm;
    mat4 mv;
    float sky_pass;
    float sky_fog_depth;
    float light_depth;
    float sky_geometry;
    vec4  view_tint;
    vec2  texture_offset;
    vec2  liquid;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 texcoords;
layout(location = 2) in vec4 color0;

layout(location = 0) out vec4 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out vec3 vpos;

void main()
{
    vec4 model_position = vec4(position, 1.0);
    vec4 vertex         = mv * model_position;

    uv    = texcoords;
    uv.xy += texture_offset;

    if (liquid.x != 0.0)
    {
        float wave_x = (position.x + position.z) * 0.0009765625 + liquid.y;
        float wave_y = position.y * 0.0009765625 + liquid.y;

        uv.x += sin(6.28318531 * wave_x) * liquid.x;
        uv.y += sin(6.28318531 * wave_y) * liquid.x;
    }

    color = color0;
    color.rgb *= view_tint.rgb;

    if (sky_pass > 0.5)
    {
        vertex = vec4(0.0, 0.0, -sky_fog_depth, 1.0);

        if (sky_geometry > 0.5)
            gl_Position = mvp * model_position;
        else
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


    vpos = vertex.xyz;
}

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
    vec4  light;
    vec4  tint;
};

layout(set = 1, binding = 1) uniform ModelLightParameters
{
    vec4 whites[32];
    vec4 lighting;
};

layout(location = 0) in vec3 position_frame1;
layout(location = 1) in vec3 position_frame2;
layout(location = 2) in vec2 texcoords;
layout(location = 3) in vec3 normal_frame1;
layout(location = 4) in vec3 normal_frame2;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 color;
layout(location = 2) out vec3 vpos;
layout(location = 3) out vec3 vnormal;

vec3 ModelLightColor(float view_depth)
{
    if (light.w > 0.5)
        return vec3(0.0);

    float level = light.x;
    float index;

    if (lighting.x > 0.5)
    {
        index = clamp(42.0 - floor(level / 6.0), 0.0, 31.0);
    }
    else
    {
        float light_level = floor(level / 4.0);
        float minimum     = clamp(36.0 - light_level, 0.0, 31.0);
        float distance    = (light.z > 0.5) ? light.y : view_depth;

        index = clamp((59.0 - light_level) - floor(1280.0 / max(1.0, distance)), minimum, 31.0);
    }

    return whites[int(index)].rgb * tint.rgb;
}

void main()
{
    vec3 blended = mix(position_frame1, position_frame2, lerp);

    vec4 model_position = model_transform * vec4(blended, 1.0);
    vec4 vertex         = mv * model_position;

    uv    = texcoords * texture_scale + texture_offset;
    color = ModelLightColor(-vertex.z);

    vec3 blended_normal = mix(normal_frame1, normal_frame2, lerp);

    vec3 model_normal = mat3(model_transform) * blended_normal;

    vnormal = normalize(mat3(mv) * model_normal);

    gl_Position = mvp * model_position;


    vpos = vertex.xyz;
}

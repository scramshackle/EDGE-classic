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
    float light_row_offset;
    float vertex_padding0;
    vec4  sprite_view0;
    vec4  sprite_view1;
};

layout(set = 1, binding = 1) uniform SpriteParameters
{
    vec4 whites[32];
    vec4 sprite_lighting;
};

layout(location = 0) in vec4 sprite_origin;
layout(location = 1) in vec4 sprite_extent;
layout(location = 2) in vec4 sprite_texture_coordinates;
layout(location = 3) in vec4 sprite_fuzz;
layout(location = 4) in vec2 sprite_light;
layout(location = 5) in vec4 sprite_color;

layout(location = 0) out vec4 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out vec3 vpos;

void main()
{
    int corner = gl_VertexIndex & 3;

    float corner_x = (corner >= 2) ? 1.0 : 0.0;
    float corner_y = (corner == 1 || corner == 2) ? 1.0 : 0.0;

    float flags  = sprite_extent.w;
    bool  fuzzy  = flags >= 2.0;
    bool  mirror = mod(flags, 2.0) >= 1.0 && sprite_view1.x > 0.5;

    float left_extent  = sprite_extent.x;
    float right_extent = sprite_extent.y;

    vec4 coordinates = sprite_texture_coordinates;

    if (mirror)
    {
        left_extent  = -sprite_extent.y;
        right_extent = -sprite_extent.x;
        coordinates  = coordinates.zyxw;
    }

    float along = mix(left_extent, right_extent, corner_x);

    vec2 ground = sprite_origin.xy + sprite_view0.xy * along + sprite_view0.zw * sprite_extent.z * (corner_y - 0.5);

    vec4 model_position = vec4(ground, mix(sprite_origin.z, sprite_origin.w, corner_y), 1.0);
    vec4 vertex         = mv * model_position;

    uv.x = mix(coordinates.x, coordinates.z, corner_x);
    uv.y = mix(coordinates.y, coordinates.w, corner_y);
    uv.z = corner_x * sprite_fuzz.x + sprite_fuzz.z;
    uv.w = corner_y * sprite_fuzz.y + sprite_fuzz.w;

    if (fuzzy)
    {
        color = sprite_color;
    }
    else
    {
        float extra = sprite_view1.y;

        if (sprite_lighting.y > 0.5 && extra <= 250.0)
            extra = 0.0;

        float level = clamp(sprite_light.x + extra, 0.0, 255.0);
        float index;

        if (sprite_lighting.x > 0.5)
        {
            index = clamp(42.0 - floor(level / 6.0), 0.0, 31.0);
        }
        else
        {
            float light    = floor(level / 4.0);
            float minimum  = clamp(36.0 - light, 0.0, 31.0);
            float distance = -vertex.z;

            index = clamp((59.0 - light) - floor(1280.0 / max(1.0, distance)), minimum, 31.0);
        }

        color = vec4(min(whites[int(index)].rgb * sprite_color.rgb, vec3(1.0)), sprite_color.a);
    }

    color.rgb *= view_tint.rgb;

    gl_Position = mvp * model_position;

    vpos = vertex.xyz;
}

uniform mat4 u_model_view_projection;
uniform mat4 u_model_view;

uniform float u_sky_pass;
uniform float u_sky_fog_depth;
uniform float u_sky_geometry;

uniform float u_light_depth;
uniform vec4  u_view_tint;

attribute vec3 a_position;
attribute vec4 a_texture_coordinates;

uniform vec2 u_texture_offset;
uniform float u_light_row_offset;
attribute vec4 a_color;

attribute vec4 a_sprite_origin;
attribute vec4 a_sprite_extent;
attribute vec4 a_sprite_fuzz;
attribute vec2 a_sprite_light;

uniform float u_sprite_mode;
uniform vec4  u_sprite_view0;
uniform vec4  u_sprite_view1;
uniform vec4  u_sprite_whites[32];
uniform vec4  u_sprite_lighting;

varying vec4 v_texture_coordinates;
varying vec4 v_color;
varying vec3 v_eye_position;

vec4 SpriteColor(float eye_depth)
{
    if (a_sprite_extent.w >= 2.0)
        return a_color;

    float extra = u_sprite_view1.y;

    if (u_sprite_lighting.y > 0.5 && extra <= 250.0)
        extra = 0.0;

    float level = clamp(a_sprite_light.x + extra, 0.0, 255.0);
    float index;

    if (u_sprite_lighting.x > 0.5)
    {
        index = clamp(42.0 - floor(level / 6.0), 0.0, 31.0);
    }
    else
    {
        float light   = floor(level / 4.0);
        float minimum = clamp(36.0 - light, 0.0, 31.0);

        index = clamp((59.0 - light) - floor(1280.0 / max(1.0, eye_depth)), minimum, 31.0);
    }

    return vec4(min(u_sprite_whites[int(index)].rgb * a_color.rgb, vec3(1.0)), a_color.a);
}

void main()
{
    vec4 model_position = vec4(a_position, 1.0);

    v_texture_coordinates = a_texture_coordinates;
    v_color               = a_color;

    if (u_sprite_mode > 0.5)
    {
        float corner_x = a_position.x;
        float corner_y = a_position.y;

        float flags  = a_sprite_extent.w;
        bool  mirror = mod(flags, 2.0) >= 1.0 && u_sprite_view1.x > 0.5;

        float left_extent  = a_sprite_extent.x;
        float right_extent = a_sprite_extent.y;

        vec4 coordinates = a_texture_coordinates;

        if (mirror)
        {
            left_extent  = -a_sprite_extent.y;
            right_extent = -a_sprite_extent.x;
            coordinates  = coordinates.zyxw;
        }

        float along = mix(left_extent, right_extent, corner_x);

        vec2 ground = a_sprite_origin.xy + u_sprite_view0.xy * along +
                      u_sprite_view0.zw * a_sprite_extent.z * (corner_y - 0.5);

        model_position = vec4(ground, mix(a_sprite_origin.z, a_sprite_origin.w, corner_y), 1.0);

        v_texture_coordinates.x = mix(coordinates.x, coordinates.z, corner_x);
        v_texture_coordinates.y = mix(coordinates.y, coordinates.w, corner_y);
        v_texture_coordinates.z = corner_x * a_sprite_fuzz.x + a_sprite_fuzz.z;
        v_texture_coordinates.w = corner_y * a_sprite_fuzz.y + a_sprite_fuzz.w;

        v_color = SpriteColor(-(u_model_view * model_position).z);
    }

    v_texture_coordinates.xy += u_texture_offset;
    v_color.rgb *= u_view_tint.rgb;

    if (u_sky_pass > 0.5)
    {
        v_eye_position = vec3(0.0, 0.0, -u_sky_fog_depth);

        if (u_sky_geometry > 0.5)
            gl_Position = u_model_view_projection * model_position;
        else
            gl_Position = vec4(a_position.xy, 1.0, 1.0);
    }
    else
    {
        v_eye_position = (u_model_view * model_position).xyz;

        if (u_light_depth > 0.5)
        {
            v_texture_coordinates.z = -v_eye_position.z * 0.000625;
            v_texture_coordinates.w = clamp(v_texture_coordinates.w + u_light_row_offset, 0.5 / 64.0, 63.5 / 64.0);
        }

        gl_Position = u_model_view_projection * model_position;
    }
}

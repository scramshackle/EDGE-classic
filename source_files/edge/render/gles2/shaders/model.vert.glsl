uniform mat4 u_model_view_projection;
uniform mat4 u_model_view;
uniform mat4 u_model_transform;

uniform float u_lerp;

uniform vec2 u_texture_scale;
uniform vec2 u_texture_offset;

uniform vec4 u_model_whites[32];
uniform vec4 u_model_lighting;
uniform vec4 u_model_light;
uniform vec4 u_model_tint;

attribute vec3 a_position_frame1;
attribute vec3 a_position_frame2;
attribute vec2 a_texture_coordinates;
attribute vec3 a_normal_frame1;
attribute vec3 a_normal_frame2;

varying vec4 v_color_and_u;
varying vec4 v_eye_and_v;
varying vec3 v_normal;

vec3 ModelLightColor(float view_depth)
{
    if (u_model_light.w > 0.5)
        return vec3(0.0);

    float level = u_model_light.x;
    float index;

    if (u_model_lighting.x > 0.5)
    {
        index = clamp(42.0 - floor(level / 6.0), 0.0, 31.0);
    }
    else
    {
        float light    = floor(level / 4.0);
        float minimum  = clamp(36.0 - light, 0.0, 31.0);
        float distance = (u_model_light.z > 0.5) ? u_model_light.y : view_depth;

        index = clamp((59.0 - light) - floor(1280.0 / max(1.0, distance)), minimum, 31.0);
    }

    return u_model_whites[int(index)].rgb * u_model_tint.rgb;
}

void main()
{
    vec3 blended = mix(a_position_frame1, a_position_frame2, u_lerp);

    vec4 model_position = u_model_transform * vec4(blended, 1.0);

    vec2 texture_coordinates = a_texture_coordinates * u_texture_scale + u_texture_offset;

    vec3 blended_normal = mix(a_normal_frame1, a_normal_frame2, u_lerp);

    vec3 model_normal = mat3(u_model_transform[0].xyz, u_model_transform[1].xyz, u_model_transform[2].xyz) *
                        blended_normal;

    vec3 eye = (u_model_view * model_position).xyz;

    v_color_and_u = vec4(ModelLightColor(-eye.z), texture_coordinates.x);
    v_eye_and_v   = vec4(eye, texture_coordinates.y);
    v_normal      = normalize(mat3(u_model_view[0].xyz, u_model_view[1].xyz, u_model_view[2].xyz) * model_normal);

    gl_Position = u_model_view_projection * model_position;
}

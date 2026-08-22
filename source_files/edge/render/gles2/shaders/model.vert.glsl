uniform mat4 u_model_view_projection;
uniform mat4 u_model_view;
uniform mat4 u_model_transform;

uniform float u_lerp;

uniform vec2 u_texture_scale;
uniform vec2 u_texture_offset;

attribute vec3 a_position_frame1;
attribute vec3 a_position_frame2;
attribute vec2 a_texture_coordinates;
attribute vec3 a_color;
attribute vec3 a_normal_frame1;
attribute vec3 a_normal_frame2;

varying vec4 v_color_and_u;
varying vec4 v_eye_and_v;
varying vec3 v_normal;

void main()
{
    vec3 blended = mix(a_position_frame1, a_position_frame2, u_lerp);

    vec4 model_position = u_model_transform * vec4(blended, 1.0);

    vec2 texture_coordinates = a_texture_coordinates * u_texture_scale + u_texture_offset;

    vec3 blended_normal = mix(a_normal_frame1, a_normal_frame2, u_lerp);

    vec3 model_normal = mat3(u_model_transform[0].xyz, u_model_transform[1].xyz, u_model_transform[2].xyz) *
                        blended_normal;

    v_color_and_u = vec4(a_color, texture_coordinates.x);
    v_eye_and_v   = vec4((u_model_view * model_position).xyz, texture_coordinates.y);
    v_normal      = normalize(mat3(u_model_view[0].xyz, u_model_view[1].xyz, u_model_view[2].xyz) * model_normal);

    gl_Position = u_model_view_projection * model_position;
}

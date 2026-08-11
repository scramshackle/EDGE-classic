#pragma once

#include <stdint.h>

#include "HandmadeMath.h"
#include "i_defs_gl.h"
#include "r_units.h"

constexpr int32_t kGles2MaximumClipPlanes = 6;

constexpr GLuint kGles2AttributePosition           = 0;
constexpr GLuint kGles2AttributeTextureCoordinates = 1;
constexpr GLuint kGles2AttributeColor              = 2;

constexpr int32_t kGles2MaximumLightsPerPass = 4;

constexpr GLuint kGles2AttributeLightPosition           = 0;
constexpr GLuint kGles2AttributeLightTextureCoordinates = 1;

constexpr GLuint kGles2AttributeModelPositionFrame1     = 0;
constexpr GLuint kGles2AttributeModelPositionFrame2     = 1;
constexpr GLuint kGles2AttributeModelTextureCoordinates = 2;
constexpr GLuint kGles2AttributeModelColor              = 3;

constexpr GLint kGles2TextureUnit0 = 0;
constexpr GLint kGles2TextureUnit1 = 1;

enum Gles2FogMode
{
    kGles2FogModeNone        = 0,
    kGles2FogModeLinear      = 1,
    kGles2FogModeExponential = 2
};

class Gles2Program
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetModelViewProjection(const HMM_Mat4 &matrix);

    void SetModelView(const HMM_Mat4 &matrix);

    void SetClipPlane(int32_t index, const double equation[4]);

    void SetClipPlaneEnabled(int32_t index, bool enabled);

    void SetMultiTexture(bool enabled);

    void SetLineMode(bool enabled);

    void SetSkipRGB(bool enabled);

    void SetAlphaTest(float reference);

    void SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start, float end);

    void SetSkyPass(const SkyPassInfo *sky_pass);

    uint32_t UniformUpdateCount() const
    {
        return uniform_update_count_;
    }

    void ResetStatistics()
    {
        uniform_update_count_ = 0;
    }

  private:
    struct Gles2ClipPlane
    {
        bool  enabled;
        float equation[4];
    };

    void UploadClipPlane(int32_t index);

    void SetFloat(GLint location, float &shadow, float value);

    GLuint program_ = 0;

    GLint uniform_model_view_projection_ = -1;
    GLint uniform_model_view_            = -1;
    GLint uniform_clip_plane_            = -1;
    GLint uniform_texture0_              = -1;
    GLint uniform_texture1_              = -1;
    GLint uniform_multi_texture_         = -1;
    GLint uniform_line_mode_             = -1;
    GLint uniform_skip_rgb_              = -1;
    GLint uniform_alpha_test_            = -1;
    GLint uniform_fog_mode_              = -1;
    GLint uniform_fog_color_             = -1;
    GLint uniform_fog_density_           = -1;
    GLint uniform_fog_start_             = -1;
    GLint uniform_fog_end_               = -1;

    GLint uniform_sky_pass_               = -1;
    GLint uniform_sky_fog_depth_          = -1;
    GLint uniform_sky_inverse_projection_ = -1;
    GLint uniform_sky_inverse_view_       = -1;
    GLint uniform_sky_viewport_           = -1;
    GLint uniform_sky_stretch_mode_       = -1;
    GLint uniform_sky_u_scale_            = -1;
    GLint uniform_sky_ty_                 = -1;
    GLint uniform_sky_u_offset_           = -1;
    GLint uniform_sky_v_offset_           = -1;
    GLint uniform_sky_vertical_fov_slope_ = -1;
    GLint uniform_sky_horizon_shift_      = -1;

    HMM_Mat4 shadow_model_view_projection_ = {};
    HMM_Mat4 shadow_model_view_            = {};

    Gles2ClipPlane clip_planes_[kGles2MaximumClipPlanes] = {};

    float shadow_multi_texture_ = -1.0f;
    float shadow_line_mode_     = -1.0f;
    float shadow_skip_rgb_      = -1.0f;
    float shadow_alpha_test_    = -1.0f;
    float shadow_fog_mode_      = -1.0f;
    float shadow_fog_density_   = -1.0f;
    float shadow_fog_start_     = -1.0f;
    float shadow_fog_end_       = -1.0f;

    float shadow_fog_color_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

    float shadow_sky_pass_ = -1.0f;

    uint32_t uniform_update_count_ = 0;
};

extern Gles2Program gles2_program;

class Gles2ModelProgram
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetMatrices(const HMM_Mat4 &model_view_projection, const HMM_Mat4 &model_view);

    void SetTransform(const HMM_Mat4 &transform);

    void SetLerp(float lerp);

    void SetTextureTransform(const HMM_Vec2 &scale, const HMM_Vec2 &offset);

    void SetAlpha(float alpha);

    void SetAlphaTest(float reference);

    void SetAdditivePass(bool additive);

    void SetClipPlane(int32_t index, bool enabled, const float equation[4]);

    void SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start, float end);

  private:
    void SetFloat(GLint location, float &shadow, float value);

    GLuint program_ = 0;

    GLint uniform_model_view_projection_ = -1;
    GLint uniform_model_view_            = -1;
    GLint uniform_model_transform_       = -1;
    GLint uniform_lerp_                  = -1;
    GLint uniform_texture_scale_         = -1;
    GLint uniform_texture_offset_        = -1;
    GLint uniform_texture0_              = -1;
    GLint uniform_alpha_                 = -1;
    GLint uniform_alpha_test_            = -1;
    GLint uniform_additive_pass_         = -1;
    GLint uniform_clip_plane_            = -1;
    GLint uniform_fog_mode_              = -1;
    GLint uniform_fog_color_             = -1;
    GLint uniform_fog_density_           = -1;
    GLint uniform_fog_start_             = -1;
    GLint uniform_fog_end_               = -1;

    float shadow_lerp_          = -1.0f;
    float shadow_alpha_         = -1.0f;
    float shadow_alpha_test_    = -1.0f;
    float shadow_additive_pass_ = -1.0f;
    float shadow_fog_mode_      = -1.0f;
    float shadow_fog_density_   = -1.0f;
    float shadow_fog_start_     = -1.0f;
    float shadow_fog_end_       = -1.0f;

    float shadow_fog_color_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
};

extern Gles2ModelProgram gles2_model_program;

class Gles2MovieProgram
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetMatrices(const HMM_Mat4 &model_view, const HMM_Mat4 &projection);

    void SetPlaneScales(float luma_x, float luma_y, float chroma_x, float chroma_y);

  private:
    GLuint program_ = 0;

    GLint uniform_model_view_   = -1;
    GLint uniform_projection_   = -1;
    GLint uniform_texture_y_    = -1;
    GLint uniform_texture_cb_   = -1;
    GLint uniform_texture_cr_   = -1;
    GLint uniform_luma_scale_   = -1;
    GLint uniform_chroma_scale_ = -1;
};

extern Gles2MovieProgram gles2_movie_program;

class Gles2LightProgram
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetModelViewProjection(const HMM_Mat4 &matrix);

    void SetSurfaceMode(float mode);

    void SetAlpha(float alpha);

    void SetAlphaTest(float reference);

    void SetSurfaceNormal(float x, float y, float z, float radius_xy_divisor, bool normal_is_horizontal);

    void SetLights(const float *position_radius, const float *color, int32_t count);

  private:
    void SetFloat(GLint location, float &shadow, float value);

    GLuint program_ = 0;

    GLint uniform_model_view_projection_ = -1;
    GLint uniform_surface_texture_       = -1;
    GLint uniform_light_texture_         = -1;
    GLint uniform_surface_mode_          = -1;
    GLint uniform_alpha_                 = -1;
    GLint uniform_alpha_test_            = -1;
    GLint uniform_surface_normal_        = -1;
    GLint uniform_normal_horizontal_     = -1;
    GLint uniform_light_count_           = -1;
    GLint uniform_light_position_radius_ = -1;
    GLint uniform_light_color_           = -1;

    float shadow_surface_mode_      = -1.0f;
    float shadow_alpha_             = -1.0f;
    float shadow_alpha_test_        = -1.0f;
    float shadow_normal_horizontal_ = -1.0f;
};

extern Gles2LightProgram gles2_light_program;

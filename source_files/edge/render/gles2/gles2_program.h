#pragma once

#include <stdint.h>

#include "epi_vector.h"
#include "i_defs_gl.h"
#include "r_units.h"


constexpr GLuint kGles2AttributePosition           = 0;
constexpr GLuint kGles2AttributeTextureCoordinates = 1;
constexpr GLuint kGles2AttributeColor              = 2;
constexpr GLuint kGles2AttributeSpriteOrigin       = 3;
constexpr GLuint kGles2AttributeSpriteExtent       = 4;
constexpr GLuint kGles2AttributeSpriteFuzz         = 5;
constexpr GLuint kGles2AttributeSpriteLight        = 6;



constexpr GLuint kGles2AttributeModelPositionFrame1     = 0;
constexpr GLuint kGles2AttributeModelPositionFrame2     = 1;
constexpr GLuint kGles2AttributeModelTextureCoordinates = 2;
constexpr GLuint kGles2AttributeModelNormalFrame1       = 4;
constexpr GLuint kGles2AttributeModelNormalFrame2       = 5;

constexpr GLint kGles2TextureUnit0 = 0;
constexpr GLint kGles2TextureUnit1 = 1;
constexpr GLint kGles2TextureUnitSkyCube = 2;
constexpr GLint kGles2TextureUnitLightData = 3;
constexpr GLint kGles2TextureUnitLightHeaders = 4;
constexpr GLint kGles2TextureUnitLightIndices = 5;
constexpr GLint kGles2TextureUnitColorLookup = 6;

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

    void SetModelViewProjection(const epi::Mat4 &matrix);

    void SetModelView(const epi::Mat4 &matrix);

    void SetMultiTexture(bool enabled);

    void SetLightFalloff(bool enabled);

    void SetWorldLit(bool enabled);

    void SetLightGrid(const struct Gles2LightGridState *grid);

    void SetGlowSet(int index);

    void SetLineMode(bool enabled);

    void SetSkipRGB(bool enabled);

    void SetAlphaTest(float reference);

    void SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start, float end);

    void SetSkyPass(const SkyPassInfo *sky_pass);

    void SetLightDepth(bool enabled);

    void SetViewTint(float r, float g, float b);

    void SetOit(float mode, float scale);

    void SetTextureOffset(const epi::Vec2 &offset);

    void SetLightRowOffset(float offset);

    void SetSpriteMode(bool enabled);

    void SetSpriteLightTable(const SpriteLightTable *light_table);

    void SetSpriteView(const epi::Vec4 view[2]);

    void SetLiquid(const epi::Vec4 &liquid);

    void SetColorLookup(int slot);

    void SetWhiten(bool enabled);

    void SetBlur(const epi::Vec4 &blur);

    float OitModeShadow() const
    {
        return shadow_oit_mode_;
    }

    void ForceOitReset();

    uint32_t UniformUpdateCount() const
    {
        return uniform_update_count_;
    }

    void ResetStatistics()
    {
        uniform_update_count_      = 0;
        shadow_glow_set_           = -2;
        shadow_sprite_light_table_ = nullptr;
    }

  private:
    void SetFloat(GLint location, float &shadow, float value);

    GLuint program_ = 0;

    GLint uniform_model_view_projection_ = -1;
    GLint uniform_model_view_            = -1;
    GLint uniform_texture0_              = -1;
    GLint uniform_texture1_              = -1;
    GLint uniform_multi_texture_         = -1;
    GLint uniform_light_falloff_         = -1;
    GLint uniform_world_lit_             = -1;
    GLint uniform_light_data_            = -1;
    GLint uniform_light_headers_         = -1;
    GLint uniform_light_indices_         = -1;
    GLint uniform_light_list_            = -1;
    GLint uniform_light_view_            = -1;
    GLint uniform_light_cluster_         = -1;
    GLint uniform_light_bounds_min_      = -1;
    GLint uniform_light_bounds_range_    = -1;
    GLint uniform_light_radius_scale_    = -1;
    GLint uniform_light_data_step_       = -1;
    GLint uniform_glow_count_            = -1;
    GLint uniform_glow_plane_            = -1;
    GLint uniform_glow_color_            = -1;
    GLint uniform_glow_additive_         = -1;
    GLint uniform_line_mode_             = -1;
    GLint uniform_skip_rgb_              = -1;
    GLint uniform_alpha_test_            = -1;
    GLint uniform_fog_mode_              = -1;
    GLint uniform_fog_color_             = -1;
    GLint uniform_fog_density_           = -1;
    GLint uniform_fog_start_             = -1;
    GLint uniform_fog_end_               = -1;

    GLint uniform_sky_pass_               = -1;
    GLint uniform_light_depth_           = -1;
    GLint uniform_view_tint_             = -1;
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
    GLint uniform_sky_geometry_ = -1;
    GLint uniform_sky_is_box_ = -1;
    GLint uniform_sky_cube_    = -1;
    GLint uniform_sky_horizon_shift_      = -1;
    GLint uniform_oit_mode_               = -1;
    GLint uniform_oit_scale_              = -1;
    GLint uniform_texture_offset_         = -1;
    GLint uniform_light_row_offset_       = -1;
    GLint uniform_sprite_mode_            = -1;
    GLint uniform_sprite_whites_          = -1;
    GLint uniform_sprite_lighting_        = -1;
    GLint uniform_sprite_view0_           = -1;
    GLint uniform_sprite_view1_           = -1;
    GLint uniform_liquid_                 = -1;
    GLint uniform_color_lookup_           = -1;
    GLint uniform_color_lookup_enabled_   = -1;
    GLint uniform_whiten_                 = -1;
    GLint uniform_blur_                   = -1;

    epi::Mat4 shadow_model_view_projection_ = {};
    epi::Mat4 shadow_model_view_            = {};

    float shadow_multi_texture_ = -1.0f;
    float shadow_light_falloff_ = -1.0f;
    float shadow_world_lit_     = -1.0f;
    int   shadow_glow_set_      = -2;
    float shadow_line_mode_     = -1.0f;
    float shadow_skip_rgb_      = -1.0f;
    float shadow_alpha_test_    = -1.0f;
    float shadow_fog_mode_      = -1.0f;
    float shadow_fog_density_   = -1.0f;
    float shadow_fog_start_     = -1.0f;
    float shadow_fog_end_       = -1.0f;

    float shadow_fog_color_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

    float shadow_sky_pass_ = -1.0f;
    float shadow_light_depth_ = -1.0f;
    float shadow_view_tint_[3] = {-1.0f, -1.0f, -1.0f};

    float shadow_oit_mode_  = -1.0f;
    float shadow_oit_scale_ = -1.0f;

    epi::Vec2 shadow_texture_offset_   = {-1.0e30f, -1.0e30f};
    float    shadow_light_row_offset_ = -1.0e30f;
    float    shadow_sprite_mode_      = -1.0f;
    epi::Vec4 shadow_sprite_view_[2]   = {epi::Vec4{-1.0e30f, 0, 0, 0}, epi::Vec4{-1.0e30f, 0, 0, 0}};

    const SpriteLightTable *shadow_sprite_light_table_ = nullptr;
    epi::Vec4               shadow_liquid_             = epi::Vec4{-1.0e30f, -1.0e30f, -1.0e30f, -1.0e30f};

    float shadow_color_lookup_enabled_ = -1.0f;
    float shadow_whiten_               = -1.0f;

    epi::Vec4 shadow_blur_ = epi::Vec4{-1.0f, -1.0f, -1.0f, -1.0f};

    uint32_t uniform_update_count_ = 0;
};

extern Gles2Program gles2_program;

class Gles2ModelProgram
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetMatrices(const epi::Mat4 &model_view_projection, const epi::Mat4 &model_view);

    void SetTransform(const epi::Mat4 &transform);

    void SetLerp(float lerp);

    void SetTextureTransform(const epi::Vec2 &scale, const epi::Vec2 &offset);

    void SetAlpha(float alpha);

    void SetAlphaTest(float reference);

    void SetLighting(const ModelDrawInfo &info);

    void SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start, float end);

    void SetOit(float mode, float scale);

    void SetWorldLit(bool enabled);

    void SetLightGrid(const struct Gles2LightGridState *grid);

    void SetGlowSet(int index);

    void SetColorLookup(int slot);

  private:
    void SetFloat(GLint location, float &shadow, float value);

    GLuint program_ = 0;

    GLint uniform_model_view_projection_ = -1;
    GLint uniform_model_view_            = -1;
    GLint uniform_model_transform_       = -1;
    GLint uniform_world_lit_             = -1;
    GLint uniform_light_data_            = -1;
    GLint uniform_light_headers_         = -1;
    GLint uniform_light_indices_         = -1;
    GLint uniform_light_view_            = -1;
    GLint uniform_light_cluster_         = -1;
    GLint uniform_light_list_            = -1;
    GLint uniform_light_bounds_min_      = -1;
    GLint uniform_light_bounds_range_    = -1;
    GLint uniform_light_radius_scale_    = -1;
    GLint uniform_light_data_step_       = -1;
    GLint uniform_glow_count_            = -1;
    GLint uniform_glow_plane_            = -1;
    GLint uniform_glow_color_            = -1;
    GLint uniform_glow_additive_         = -1;
    GLint uniform_lerp_                  = -1;
    GLint uniform_texture_scale_         = -1;
    GLint uniform_texture_offset_        = -1;
    GLint uniform_texture0_              = -1;
    GLint uniform_alpha_                 = -1;
    GLint uniform_alpha_test_            = -1;
    GLint uniform_model_whites_          = -1;
    GLint uniform_model_lighting_        = -1;
    GLint uniform_model_light_           = -1;
    GLint uniform_model_tint_            = -1;
    GLint uniform_fog_mode_              = -1;
    GLint uniform_fog_color_             = -1;
    GLint uniform_fog_density_           = -1;
    GLint uniform_fog_start_             = -1;
    GLint uniform_fog_end_               = -1;
    GLint uniform_oit_mode_              = -1;
    GLint uniform_oit_scale_             = -1;
    GLint uniform_color_lookup_          = -1;
    GLint uniform_color_lookup_enabled_  = -1;

    float shadow_lerp_        = -1.0f;
    float shadow_alpha_       = -1.0f;
    float shadow_alpha_test_  = -1.0f;
    float shadow_fog_mode_    = -1.0f;
    float shadow_fog_density_ = -1.0f;
    float shadow_fog_start_   = -1.0f;
    float shadow_fog_end_     = -1.0f;

    float shadow_fog_color_[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

    float shadow_oit_mode_  = -1.0f;
    float shadow_oit_scale_ = -1.0f;
    float shadow_world_lit_ = -1.0f;
    int   shadow_glow_set_  = -2;

    float shadow_color_lookup_enabled_ = -1.0f;
};

extern Gles2ModelProgram gles2_model_program;

void Gles2UploadColorLookup(int slot, const uint8_t *pixels);

class Gles2MovieProgram
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetMatrices(const epi::Mat4 &model_view, const epi::Mat4 &projection);

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

class Gles2OitProgram
{
  public:
    bool Init();

    void Shutdown();

    void Use();

    void SetScale(float scale);

  private:
    GLuint program_ = 0;

    GLint uniform_accumulation_ = -1;
    GLint uniform_revealage_    = -1;
    GLint uniform_scale_        = -1;

    float shadow_scale_ = -1.0f;
};

extern Gles2OitProgram gles2_oit_program;

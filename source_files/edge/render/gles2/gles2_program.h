#pragma once

#include <stdint.h>

#include "HandmadeMath.h"
#include "i_defs_gl.h"

constexpr int32_t kGles2MaximumClipPlanes = 6;

constexpr GLuint kGles2AttributePosition           = 0;
constexpr GLuint kGles2AttributeTextureCoordinates = 1;
constexpr GLuint kGles2AttributeColor              = 2;

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

    uint32_t uniform_update_count_ = 0;
};

extern Gles2Program gles2_program;

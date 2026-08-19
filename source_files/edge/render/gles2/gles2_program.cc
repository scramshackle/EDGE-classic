#include "gles2_program.h"

#include <string.h>

#include "epi.h"
#include "epi_math.h"
#include "i_system.h"
#include "r_backend.h"
#include "shaders/light_glsl.h"
#include "shaders/model_glsl.h"
#include "shaders/movie_glsl.h"
#include "shaders/oit_glsl.h"
#include "shaders/world_glsl.h"

Gles2Program      gles2_program;
Gles2ModelProgram gles2_model_program;
Gles2MovieProgram gles2_movie_program;
Gles2LightProgram gles2_light_program;
Gles2OitProgram   gles2_oit_program;

static GLuint CompileStage(GLenum stage, const char *source, const char *label)
{
    GLuint shader = glCreateShader(stage);

    if (!shader)
    {
        FatalError("Gles2Program: glCreateShader failed for %s\n", label);
    }

    const char *strings[2] = {Gles2ShaderPreamble(stage == GL_FRAGMENT_SHADER), source};

    glShaderSource(shader, 2, strings, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (compiled != GL_TRUE)
    {
        GLint log_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetShaderInfoLog(shader, log_length, nullptr, log);
        }

        FatalError("Gles2Program: %s failed to compile:\n%s\n", label, log);
    }

    return shader;
}

bool Gles2Program::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kWorldVertexSource, "world.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kWorldFragmentSource, "world.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
    {
        FatalError("Gles2Program: glCreateProgram failed\n");
    }

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributePosition, "a_position");
    glBindAttribLocation(program_, kGles2AttributeTextureCoordinates, "a_texture_coordinates");
    glBindAttribLocation(program_, kGles2AttributeColor, "a_color");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetProgramInfoLog(program_, log_length, nullptr, log);
        }

        FatalError("Gles2Program: world program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_model_view_projection_ = glGetUniformLocation(program_, "u_model_view_projection");
    uniform_model_view_            = glGetUniformLocation(program_, "u_model_view");
    uniform_texture0_              = glGetUniformLocation(program_, "u_texture0");
    uniform_texture1_              = glGetUniformLocation(program_, "u_texture1");
    uniform_multi_texture_         = glGetUniformLocation(program_, "u_multi_texture");
    uniform_line_mode_             = glGetUniformLocation(program_, "u_line_mode");
    uniform_skip_rgb_              = glGetUniformLocation(program_, "u_skip_rgb");
    uniform_alpha_test_            = glGetUniformLocation(program_, "u_alpha_test");
    uniform_fog_mode_              = glGetUniformLocation(program_, "u_fog_mode");
    uniform_fog_color_             = glGetUniformLocation(program_, "u_fog_color");
    uniform_fog_density_           = glGetUniformLocation(program_, "u_fog_density");
    uniform_fog_start_             = glGetUniformLocation(program_, "u_fog_start");
    uniform_fog_end_               = glGetUniformLocation(program_, "u_fog_end");

    uniform_sky_pass_               = glGetUniformLocation(program_, "u_sky_pass");
    uniform_oit_mode_               = glGetUniformLocation(program_, "u_oit_mode");
    uniform_oit_scale_              = glGetUniformLocation(program_, "u_oit_scale");
    uniform_texture_offset_         = glGetUniformLocation(program_, "u_texture_offset");
    uniform_liquid_                 = glGetUniformLocation(program_, "u_liquid");
    uniform_light_depth_            = glGetUniformLocation(program_, "u_light_depth");
    uniform_view_tint_              = glGetUniformLocation(program_, "u_view_tint");
    uniform_sky_fog_depth_          = glGetUniformLocation(program_, "u_sky_fog_depth");
    uniform_sky_inverse_projection_ = glGetUniformLocation(program_, "u_sky_inverse_projection");
    uniform_sky_inverse_view_       = glGetUniformLocation(program_, "u_sky_inverse_view");
    uniform_sky_viewport_           = glGetUniformLocation(program_, "u_sky_viewport");
    uniform_sky_stretch_mode_       = glGetUniformLocation(program_, "u_sky_stretch_mode");
    uniform_sky_u_scale_            = glGetUniformLocation(program_, "u_sky_u_scale");
    uniform_sky_ty_                 = glGetUniformLocation(program_, "u_sky_ty");
    uniform_sky_u_offset_           = glGetUniformLocation(program_, "u_sky_u_offset");
    uniform_sky_v_offset_           = glGetUniformLocation(program_, "u_sky_v_offset");
    uniform_sky_vertical_fov_slope_ = glGetUniformLocation(program_, "u_sky_vertical_fov_slope");
    uniform_sky_horizon_shift_      = glGetUniformLocation(program_, "u_sky_horizon_shift");
    uniform_sky_is_box_             = glGetUniformLocation(program_, "u_sky_is_box");
    uniform_sky_geometry_           = glGetUniformLocation(program_, "u_sky_geometry");
    uniform_sky_cube_               = glGetUniformLocation(program_, "u_sky_cube");

    glUseProgram(program_);

    glUniform1i(uniform_texture0_, kGles2TextureUnit0);
    glUniform1i(uniform_texture1_, kGles2TextureUnit1);
    glUniform1i(uniform_sky_cube_, kGles2TextureUnitSkyCube);

    return true;
}

void Gles2Program::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2Program::Use()
{
    glUseProgram(program_);
}

void Gles2Program::SetFloat(GLint location, float &shadow, float value)
{
    if (epi::AlmostEquals(shadow, value))
    {
        return;
    }

    shadow = value;

    glUniform1f(location, value);

    uniform_update_count_++;
}

void Gles2Program::SetModelViewProjection(const HMM_Mat4 &matrix)
{
    if (memcmp(&shadow_model_view_projection_, &matrix, sizeof(HMM_Mat4)) == 0)
    {
        return;
    }

    shadow_model_view_projection_ = matrix;

    glUniformMatrix4fv(uniform_model_view_projection_, 1, GL_FALSE, (const GLfloat *)&matrix);

    uniform_update_count_++;
}

void Gles2Program::SetModelView(const HMM_Mat4 &matrix)
{
    if (memcmp(&shadow_model_view_, &matrix, sizeof(HMM_Mat4)) == 0)
    {
        return;
    }

    shadow_model_view_ = matrix;

    glUniformMatrix4fv(uniform_model_view_, 1, GL_FALSE, (const GLfloat *)&matrix);

    uniform_update_count_++;
}

void Gles2Program::SetMultiTexture(bool enabled)
{
    SetFloat(uniform_multi_texture_, shadow_multi_texture_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetLineMode(bool enabled)
{
    SetFloat(uniform_line_mode_, shadow_line_mode_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetSkipRGB(bool enabled)
{
    SetFloat(uniform_skip_rgb_, shadow_skip_rgb_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetAlphaTest(float reference)
{
    SetFloat(uniform_alpha_test_, shadow_alpha_test_, reference);
}

void Gles2Program::SetViewTint(float r, float g, float b)
{
    if (uniform_view_tint_ < 0)
        return;

    if (shadow_view_tint_[0] == r && shadow_view_tint_[1] == g && shadow_view_tint_[2] == b)
        return;

    shadow_view_tint_[0] = r;
    shadow_view_tint_[1] = g;
    shadow_view_tint_[2] = b;

    glUniform4f(uniform_view_tint_, r, g, b, 1.0f);

    uniform_update_count_++;
}

void Gles2Program::SetLightDepth(bool enabled)
{
    SetFloat(uniform_light_depth_, shadow_light_depth_, enabled ? 1.0f : 0.0f);
}

void Gles2Program::SetOit(float mode, float scale)
{
    SetFloat(uniform_oit_mode_, shadow_oit_mode_, mode);
    SetFloat(uniform_oit_scale_, shadow_oit_scale_, scale);
}

void Gles2Program::SetTextureOffset(const HMM_Vec2 &offset)
{
    if (epi::AlmostEquals(shadow_texture_offset_.X, offset.X) && epi::AlmostEquals(shadow_texture_offset_.Y, offset.Y))
        return;

    shadow_texture_offset_ = offset;

    glUniform2f(uniform_texture_offset_, offset.X, offset.Y);
}

void Gles2Program::SetLiquid(const HMM_Vec2 &liquid)
{
    if (epi::AlmostEquals(shadow_liquid_.X, liquid.X) && epi::AlmostEquals(shadow_liquid_.Y, liquid.Y))
        return;

    shadow_liquid_ = liquid;

    glUniform2f(uniform_liquid_, liquid.X, liquid.Y);
}

void Gles2Program::ForceOitReset()
{
    shadow_oit_mode_ = 0.0f;

    glUniform1f(uniform_oit_mode_, 0.0f);
}

void Gles2Program::SetSkyPass(const SkyPassInfo *sky_pass)
{
    if (!sky_pass)
    {
        SetFloat(uniform_sky_pass_, shadow_sky_pass_, 0.0f);
        return;
    }

    glUniform1f(uniform_sky_is_box_, sky_pass->is_box ? 1.0f : 0.0f);
    glUniform1f(uniform_sky_geometry_, sky_pass->is_geometry ? 1.0f : 0.0f);

    if (sky_pass->is_box && sky_pass->cube_texture)
    {
        glActiveTexture(GL_TEXTURE0 + kGles2TextureUnitSkyCube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, sky_pass->cube_texture);
        glActiveTexture(GL_TEXTURE0 + kGles2TextureUnit0);
    }

    SetFloat(uniform_sky_pass_, shadow_sky_pass_, 1.0f);

    glUniformMatrix4fv(uniform_sky_inverse_projection_, 1, GL_FALSE, (const GLfloat *)&sky_pass->inverse_projection);
    glUniformMatrix4fv(uniform_sky_inverse_view_, 1, GL_FALSE, (const GLfloat *)&sky_pass->inverse_view);

    float scale_x = render_backend->ActiveScaleX();
    float scale_y = render_backend->ActiveScaleY();

    glUniform4f(uniform_sky_viewport_, sky_pass->viewport_origin.X * scale_x, sky_pass->viewport_origin.Y * scale_y,
                sky_pass->viewport_size.X * scale_x, sky_pass->viewport_size.Y * scale_y);

    glUniform1f(uniform_sky_stretch_mode_, (float)sky_pass->stretch_mode);
    glUniform1f(uniform_sky_u_scale_, sky_pass->u_scale);
    glUniform1f(uniform_sky_ty_, sky_pass->ty);
    glUniform1f(uniform_sky_u_offset_, sky_pass->u_offset);
    glUniform1f(uniform_sky_v_offset_, sky_pass->v_offset);
    glUniform1f(uniform_sky_vertical_fov_slope_, sky_pass->vertical_fov_slope);
    glUniform1f(uniform_sky_horizon_shift_, sky_pass->horizon_shift);
    glUniform1f(uniform_sky_fog_depth_, sky_pass->fog_depth);

    uniform_update_count_ += 11;
}

void Gles2Program::SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start, float end)
{
    SetFloat(uniform_fog_mode_, shadow_fog_mode_, (float)mode);

    if (mode == kGles2FogModeNone)
    {
        return;
    }

    if (!epi::AlmostEquals(shadow_fog_color_[0], red) || !epi::AlmostEquals(shadow_fog_color_[1], green) ||
        !epi::AlmostEquals(shadow_fog_color_[2], blue))
    {
        shadow_fog_color_[0] = red;
        shadow_fog_color_[1] = green;
        shadow_fog_color_[2] = blue;
        shadow_fog_color_[3] = 1.0f;

        glUniform4f(uniform_fog_color_, red, green, blue, 1.0f);

        uniform_update_count_++;
    }

    SetFloat(uniform_fog_density_, shadow_fog_density_, density);
    SetFloat(uniform_fog_start_, shadow_fog_start_, start);
    SetFloat(uniform_fog_end_, shadow_fog_end_, end);
}

bool Gles2ModelProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kModelVertexSource, "model.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kModelFragmentSource, "model.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
    {
        FatalError("Gles2ModelProgram: glCreateProgram failed\n");
    }

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributeModelPositionFrame1, "a_position_frame1");
    glBindAttribLocation(program_, kGles2AttributeModelPositionFrame2, "a_position_frame2");
    glBindAttribLocation(program_, kGles2AttributeModelTextureCoordinates, "a_texture_coordinates");
    glBindAttribLocation(program_, kGles2AttributeModelColor, "a_color");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetProgramInfoLog(program_, log_length, nullptr, log);
        }

        FatalError("Gles2ModelProgram: model program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_model_view_projection_ = glGetUniformLocation(program_, "u_model_view_projection");
    uniform_model_view_            = glGetUniformLocation(program_, "u_model_view");
    uniform_model_transform_       = glGetUniformLocation(program_, "u_model_transform");
    uniform_lerp_                  = glGetUniformLocation(program_, "u_lerp");
    uniform_texture_scale_         = glGetUniformLocation(program_, "u_texture_scale");
    uniform_texture_offset_        = glGetUniformLocation(program_, "u_texture_offset");
    uniform_texture0_              = glGetUniformLocation(program_, "u_texture0");
    uniform_alpha_                 = glGetUniformLocation(program_, "u_alpha");
    uniform_alpha_test_            = glGetUniformLocation(program_, "u_alpha_test");
    uniform_additive_pass_         = glGetUniformLocation(program_, "u_additive_pass");
    uniform_fog_mode_              = glGetUniformLocation(program_, "u_fog_mode");
    uniform_fog_color_             = glGetUniformLocation(program_, "u_fog_color");
    uniform_fog_density_           = glGetUniformLocation(program_, "u_fog_density");
    uniform_fog_start_             = glGetUniformLocation(program_, "u_fog_start");
    uniform_fog_end_               = glGetUniformLocation(program_, "u_fog_end");
    uniform_oit_mode_              = glGetUniformLocation(program_, "u_oit_mode");
    uniform_oit_scale_             = glGetUniformLocation(program_, "u_oit_scale");

    glUseProgram(program_);
    glUniform1i(uniform_texture0_, kGles2TextureUnit0);

    return true;
}

void Gles2ModelProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2ModelProgram::Use()
{
    glUseProgram(program_);
}

void Gles2ModelProgram::SetFloat(GLint location, float &shadow, float value)
{
    if (epi::AlmostEquals(shadow, value))
    {
        return;
    }

    shadow = value;

    glUniform1f(location, value);
}

void Gles2ModelProgram::SetMatrices(const HMM_Mat4 &model_view_projection, const HMM_Mat4 &model_view)
{
    glUniformMatrix4fv(uniform_model_view_projection_, 1, GL_FALSE, (const GLfloat *)&model_view_projection);
    glUniformMatrix4fv(uniform_model_view_, 1, GL_FALSE, (const GLfloat *)&model_view);
}

void Gles2ModelProgram::SetTransform(const HMM_Mat4 &transform)
{
    glUniformMatrix4fv(uniform_model_transform_, 1, GL_FALSE, (const GLfloat *)&transform);
}

void Gles2ModelProgram::SetLerp(float lerp)
{
    SetFloat(uniform_lerp_, shadow_lerp_, lerp);
}

void Gles2ModelProgram::SetTextureTransform(const HMM_Vec2 &scale, const HMM_Vec2 &offset)
{
    glUniform2f(uniform_texture_scale_, scale.X, scale.Y);
    glUniform2f(uniform_texture_offset_, offset.X, offset.Y);
}

void Gles2ModelProgram::SetAlpha(float alpha)
{
    SetFloat(uniform_alpha_, shadow_alpha_, alpha);
}

void Gles2ModelProgram::SetAlphaTest(float reference)
{
    SetFloat(uniform_alpha_test_, shadow_alpha_test_, reference);
}

void Gles2ModelProgram::SetAdditivePass(bool additive)
{
    SetFloat(uniform_additive_pass_, shadow_additive_pass_, additive ? 1.0f : 0.0f);
}

void Gles2ModelProgram::SetOit(float mode, float scale)
{
    SetFloat(uniform_oit_mode_, shadow_oit_mode_, mode);
    SetFloat(uniform_oit_scale_, shadow_oit_scale_, scale);
}

void Gles2ModelProgram::SetFog(Gles2FogMode mode, float red, float green, float blue, float density, float start,
                               float end)
{
    SetFloat(uniform_fog_mode_, shadow_fog_mode_, (float)mode);

    if (mode == kGles2FogModeNone)
    {
        return;
    }

    if (!epi::AlmostEquals(shadow_fog_color_[0], red) || !epi::AlmostEquals(shadow_fog_color_[1], green) ||
        !epi::AlmostEquals(shadow_fog_color_[2], blue))
    {
        shadow_fog_color_[0] = red;
        shadow_fog_color_[1] = green;
        shadow_fog_color_[2] = blue;
        shadow_fog_color_[3] = 1.0f;

        glUniform4f(uniform_fog_color_, red, green, blue, 1.0f);
    }

    SetFloat(uniform_fog_density_, shadow_fog_density_, density);
    SetFloat(uniform_fog_start_, shadow_fog_start_, start);
    SetFloat(uniform_fog_end_, shadow_fog_end_, end);
}

bool Gles2MovieProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kMovieVertexSource, "movie.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kMovieFragmentSource, "movie.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
        FatalError("Gles2MovieProgram: glCreateProgram failed\n");

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributePosition, "a_position");
    glBindAttribLocation(program_, kGles2AttributeTextureCoordinates, "a_texture_coordinates");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
            glGetProgramInfoLog(program_, log_length, nullptr, log);

        FatalError("Gles2MovieProgram: movie program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_model_view_   = glGetUniformLocation(program_, "u_model_view");
    uniform_projection_   = glGetUniformLocation(program_, "u_projection");
    uniform_texture_y_    = glGetUniformLocation(program_, "u_texture_y");
    uniform_texture_cb_   = glGetUniformLocation(program_, "u_texture_cb");
    uniform_texture_cr_   = glGetUniformLocation(program_, "u_texture_cr");
    uniform_luma_scale_   = glGetUniformLocation(program_, "u_luma_scale");
    uniform_chroma_scale_ = glGetUniformLocation(program_, "u_chroma_scale");

    glUseProgram(program_);

    glUniform1i(uniform_texture_y_, 0);
    glUniform1i(uniform_texture_cb_, 1);
    glUniform1i(uniform_texture_cr_, 2);

    return true;
}

void Gles2MovieProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2MovieProgram::Use()
{
    glUseProgram(program_);
}

void Gles2MovieProgram::SetMatrices(const HMM_Mat4 &model_view, const HMM_Mat4 &projection)
{
    glUniformMatrix4fv(uniform_model_view_, 1, GL_FALSE, (const GLfloat *)&model_view);
    glUniformMatrix4fv(uniform_projection_, 1, GL_FALSE, (const GLfloat *)&projection);
}

void Gles2MovieProgram::SetPlaneScales(float luma_x, float luma_y, float chroma_x, float chroma_y)
{
    glUniform2f(uniform_luma_scale_, luma_x, luma_y);
    glUniform2f(uniform_chroma_scale_, chroma_x, chroma_y);
}

bool Gles2LightProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kLightVertexSource, "light.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kLightFragmentSource, "light.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
    {
        FatalError("Gles2LightProgram: glCreateProgram failed\n");
    }

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributeLightPosition, "a_position");
    glBindAttribLocation(program_, kGles2AttributeLightTextureCoordinates, "a_texture_coordinates");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
        {
            glGetProgramInfoLog(program_, log_length, nullptr, log);
        }

        FatalError("Gles2LightProgram: light program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_model_view_projection_ = glGetUniformLocation(program_, "u_model_view_projection");
    uniform_surface_texture_       = glGetUniformLocation(program_, "u_surface_texture");
    uniform_light_texture_         = glGetUniformLocation(program_, "u_light_texture");
    uniform_surface_mode_          = glGetUniformLocation(program_, "u_surface_mode");
    uniform_alpha_                 = glGetUniformLocation(program_, "u_alpha");
    uniform_alpha_test_            = glGetUniformLocation(program_, "u_alpha_test");
    uniform_surface_normal_        = glGetUniformLocation(program_, "u_surface_normal");
    uniform_normal_horizontal_     = glGetUniformLocation(program_, "u_normal_horizontal");
    uniform_light_count_           = glGetUniformLocation(program_, "u_light_count");
    uniform_light_position_radius_ = glGetUniformLocation(program_, "u_light_position_radius");
    uniform_light_color_           = glGetUniformLocation(program_, "u_light_color");

    glUseProgram(program_);

    glUniform1i(uniform_surface_texture_, kGles2TextureUnit0);
    glUniform1i(uniform_light_texture_, kGles2TextureUnit1);

    return true;
}

void Gles2LightProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2LightProgram::Use()
{
    glUseProgram(program_);
}

void Gles2LightProgram::SetFloat(GLint location, float &shadow, float value)
{
    if (epi::AlmostEquals(shadow, value))
    {
        return;
    }

    shadow = value;

    glUniform1f(location, value);
}

void Gles2LightProgram::SetModelViewProjection(const HMM_Mat4 &matrix)
{
    glUniformMatrix4fv(uniform_model_view_projection_, 1, GL_FALSE, (const GLfloat *)&matrix);
}

void Gles2LightProgram::SetSurfaceMode(float mode)
{
    SetFloat(uniform_surface_mode_, shadow_surface_mode_, mode);
}

void Gles2LightProgram::SetAlpha(float alpha)
{
    SetFloat(uniform_alpha_, shadow_alpha_, alpha);
}

void Gles2LightProgram::SetAlphaTest(float reference)
{
    SetFloat(uniform_alpha_test_, shadow_alpha_test_, reference);
}

void Gles2LightProgram::SetSurfaceNormal(float x, float y, float z, float radius_xy_divisor,
                                         bool normal_is_horizontal)
{
    glUniform4f(uniform_surface_normal_, x, y, z, radius_xy_divisor);

    SetFloat(uniform_normal_horizontal_, shadow_normal_horizontal_, normal_is_horizontal ? 1.0f : 0.0f);
}

void Gles2LightProgram::SetLights(const float *position_radius, const float *color, int32_t count)
{
    if (count > kGles2MaximumLightsPerPass)
        count = kGles2MaximumLightsPerPass;

    glUniform4fv(uniform_light_position_radius_, kGles2MaximumLightsPerPass, position_radius);
    glUniform4fv(uniform_light_color_, kGles2MaximumLightsPerPass, color);

    glUniform1i(uniform_light_count_, count);
}

bool Gles2OitProgram::Init()
{
    GLuint vertex_shader   = CompileStage(GL_VERTEX_SHADER, kOitVertexSource, "oit.vert.glsl");
    GLuint fragment_shader = CompileStage(GL_FRAGMENT_SHADER, kOitFragmentSource, "oit.frag.glsl");

    program_ = glCreateProgram();

    if (!program_)
    {
        FatalError("Gles2OitProgram: glCreateProgram failed\n");
    }

    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);

    glBindAttribLocation(program_, kGles2AttributePosition, "a_position");
    glBindAttribLocation(program_, kGles2AttributeTextureCoordinates, "a_texture_coordinates");

    glLinkProgram(program_);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_length);

        char *log = (char *)calloc((size_t)(log_length > 1 ? log_length : 1), 1);

        if (log_length > 1)
            glGetProgramInfoLog(program_, log_length, nullptr, log);

        FatalError("Gles2OitProgram: composite program failed to link:\n%s\n", log);
    }

    glDetachShader(program_, vertex_shader);
    glDetachShader(program_, fragment_shader);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    uniform_accumulation_ = glGetUniformLocation(program_, "u_accumulation");
    uniform_revealage_    = glGetUniformLocation(program_, "u_revealage");
    uniform_scale_        = glGetUniformLocation(program_, "u_oit_scale");

    glUseProgram(program_);

    glUniform1i(uniform_accumulation_, kGles2TextureUnit0);
    glUniform1i(uniform_revealage_, kGles2TextureUnit1);

    return true;
}

void Gles2OitProgram::Shutdown()
{
    if (program_)
    {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Gles2OitProgram::Use()
{
    glUseProgram(program_);
}

void Gles2OitProgram::SetScale(float scale)
{
    if (epi::AlmostEquals(shadow_scale_, scale))
        return;

    shadow_scale_ = scale;

    glUniform1f(uniform_scale_, scale);
}

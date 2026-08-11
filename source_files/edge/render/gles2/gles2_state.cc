#include <unordered_map>

#include "epi.h"
#include "gles2_immediate.h"
#include "gles2_program.h"
#include "i_system.h"
#include "r_backend.h"
#include "r_state.h"
#include "r_units.h"

std::unordered_map<GLuint, GLint> texture_clamp_s;
std::unordered_map<GLuint, GLint> texture_clamp_t;

static bool gles2_stencil_available = false;

void Gles2DetectStencilBuffer()
{
    if (gles2_immediate.RenderTargetReady())
    {
        gles2_stencil_available = gles2_immediate.RenderTargetHasStencil();
        return;
    }

    GLint stencil_bits = 0;

    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);

    gles2_stencil_available = (stencil_bits > 0);
}

static GLint Gles2WrapMode(GLint wrap)
{
    if (wrap == GL_CLAMP || wrap == GL_CLAMP_TO_EDGE)
    {
        return GL_CLAMP_TO_EDGE;
    }

    return GL_REPEAT;
}

class Gles2RenderState : public RenderState
{
  public:
    void Enable(GLenum cap, bool enabled = true)
    {
        switch (cap)
        {
        case GL_TEXTURE_2D:
            enable_texture_2d_[active_texture_ - GL_TEXTURE0] = enabled;
            state_dirty_                                      = true;
            break;
        case GL_FOG:
            enable_fog_  = enabled;
            state_dirty_ = true;
            break;
        case GL_ALPHA_TEST:
            enable_alpha_test_ = enabled;
            state_dirty_       = true;
            break;
        case GL_BLEND:
            enable_blend_ = enabled;
            state_dirty_  = true;
            break;
        case GL_CULL_FACE:
            enable_cull_face_ = enabled;
            state_dirty_      = true;
            break;
        case GL_SCISSOR_TEST:
            enable_scissor_test_ = enabled;
            state_dirty_         = true;
            break;
        case GL_DEPTH_TEST:
            enable_depth_test_ = enabled;
            state_dirty_       = true;
            break;
        case GL_STENCIL_TEST:
            if (enabled)
            {
                glEnable(GL_STENCIL_TEST);
            }
            else
            {
                glDisable(GL_STENCIL_TEST);
            }
            break;
        case GL_CLIP_PLANE0:
        case GL_CLIP_PLANE1:
        case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3:
        case GL_CLIP_PLANE4:
        case GL_CLIP_PLANE5:
            clip_plane_enabled_[cap - GL_CLIP_PLANE0] = enabled;
            state_dirty_                              = true;
            break;
        default:
            break;
        }
    }

    void Disable(GLenum cap)
    {
        Enable(cap, false);
    }

    void DepthMask(bool enable)
    {
        depth_mask_  = enable;
        state_dirty_ = true;
    }

    void DepthFunction(GLenum func)
    {
        depth_function_ = func;
        state_dirty_    = true;
    }

    void ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
    {
        glColorMask(red, green, blue, alpha);
    }

    void StencilFunction(GLenum func, GLint ref, GLuint mask)
    {
        glStencilFunc(func, ref, mask);
    }

    void StencilOperation(GLenum stencil_fail, GLenum depth_fail, GLenum depth_pass)
    {
        glStencilOp(stencil_fail, depth_fail, depth_pass);
    }

    void StencilWriteMask(GLuint mask)
    {
        stencil_write_mask_ = mask;

        glStencilMask(mask);
    }

    bool HasStencilBuffer()
    {
        return gles2_stencil_available;
    }

    void CullFace(GLenum mode)
    {
        cull_face_   = mode;
        state_dirty_ = true;
    }

    void AlphaFunction(GLenum func, GLfloat ref)
    {
        alpha_function_           = func;
        alpha_function_reference_ = ref;
        state_dirty_              = true;
    }

    void ActiveTexture(GLenum activeTexture)
    {
        active_texture_ = activeTexture;
    }

    void BindTexture(GLuint textureid)
    {
        GLuint index = active_texture_ - GL_TEXTURE0;

        bind_texture_2d_[index] = textureid;

        state_dirty_ = true;

        if (gl_bound_texture_[index] == textureid)
        {
            return;
        }

        gl_bound_texture_[index] = textureid;

        glActiveTexture(GL_TEXTURE0 + index);
        glBindTexture(GL_TEXTURE_2D, textureid);
    }

    void ClipPlane(GLenum plane, GLdouble *equation)
    {
        int32_t index = (int32_t)(plane - GL_CLIP_PLANE0);

        for (int32_t i = 0; i < 4; i++)
        {
            clip_plane_equation_[index][i] = equation[i];
        }

        state_dirty_ = true;
    }

    void PolygonOffset(GLfloat factor, GLfloat units)
    {
        polygon_offset_factor_ = factor;
        polygon_offset_units_  = units;
        state_dirty_           = true;
    }

    void Clear(GLbitfield mask)
    {
        if (mask & GL_DEPTH_BUFFER_BIT)
        {
            glDepthMask(GL_TRUE);
            depth_mask_  = true;
            state_dirty_ = true;
        }

        if ((mask & GL_STENCIL_BUFFER_BIT) && stencil_write_mask_ != 0xFF)
        {
            glStencilMask(0xFF);
            stencil_write_mask_ = 0xFF;
        }

        glClear(mask);
    }

    void ClearColor(RGBAColor color)
    {
        if (color == clear_color_)
        {
            return;
        }

        clear_color_ = color;

        glClearColor(epi::GetRGBARed(clear_color_) / 255.0f, epi::GetRGBAGreen(clear_color_) / 255.0f,
                     epi::GetRGBABlue(clear_color_) / 255.0f, epi::GetRGBAAlpha(clear_color_) / 255.0f);
    }

    void FogMode(GLint fogMode)
    {
        fog_mode_    = fogMode;
        state_dirty_ = true;
    }

    void FogColor(RGBAColor color)
    {
        fog_color_   = color;
        state_dirty_ = true;
    }

    void FogStart(GLfloat start)
    {
        fog_start_   = start;
        state_dirty_ = true;
    }

    void FogEnd(GLfloat end)
    {
        fog_end_     = end;
        state_dirty_ = true;
    }

    void FogDensity(GLfloat density)
    {
        fog_density_ = density;
        state_dirty_ = true;
    }

    void BlendFunction(GLenum sfactor, GLenum dfactor)
    {
        blend_source_factor_      = sfactor;
        blend_destination_factor_ = dfactor;
        state_dirty_              = true;
    }

    void TextureEnvironmentMode(GLint param)
    {
        texture_environment_mode_[active_texture_ - GL_TEXTURE0] = param;
        state_dirty_                                             = true;
    }

    void TextureEnvironmentCombineRGB(GLint param)
    {
        texture_environment_combine_rgb_[active_texture_ - GL_TEXTURE0] = param;
        state_dirty_                                                    = true;
    }

    void TextureEnvironmentSource0RGB(GLint param)
    {
        texture_environment_source_0_rgb_[active_texture_ - GL_TEXTURE0] = param;
        state_dirty_                                                     = true;
    }

    void TextureMinFilter(GLint param)
    {
        glActiveTexture(active_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, param);
    }

    void TextureMagFilter(GLint param)
    {
        glActiveTexture(active_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, param);
    }

    void TextureWrapS(GLint param)
    {
        glActiveTexture(active_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, Gles2WrapMode(param));
    }

    void TextureWrapT(GLint param)
    {
        glActiveTexture(active_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, Gles2WrapMode(param));
    }

    void Hint(GLenum target, GLenum mode)
    {
        EPI_UNUSED(target);
        EPI_UNUSED(mode);
    }

    void LineWidth(float width)
    {
        line_width_ = width;
    }

    float GetLineWidth()
    {
        return line_width_;
    }

    void DeleteTexture(const GLuint *tex_id)
    {
        if (tex_id && *tex_id > 0)
        {
            texture_clamp_s.erase(*tex_id);
            texture_clamp_t.erase(*tex_id);

            glDeleteTextures(1, tex_id);

            bind_texture_2d_[0]  = 0;
            bind_texture_2d_[1]  = 0;
            gl_bound_texture_[0] = 0;
            gl_bound_texture_[1] = 0;

            state_dirty_ = true;
        }
    }

    void FrontFace(GLenum wind)
    {
        if (front_face_ == wind)
        {
            return;
        }

        front_face_ = wind;

        glFrontFace(wind);
    }

    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        scissor_x_      = x;
        scissor_y_      = y;
        scissor_width_  = width;
        scissor_height_ = height;

        glScissor(render_backend->ScaleToRenderTargetX(x), render_backend->ScaleToRenderTargetY(y),
                  render_backend->ScaleToRenderTargetX(width), render_backend->ScaleToRenderTargetY(height));
    }

    bool ScissorTestEnabled()
    {
        return enable_scissor_test_;
    }

    void GetScissor(GLint &x, GLint &y, GLsizei &width, GLsizei &height)
    {
        x      = scissor_x_;
        y      = scissor_y_;
        width  = scissor_width_;
        height = scissor_height_;
    }

    void GenTextures(GLsizei n, GLuint *textures)
    {
        glGenTextures(n, textures);
    }

    void FinishTextures(GLsizei n, GLuint *textures)
    {
        EPI_UNUSED(n);
        EPI_UNUSED(textures);
    }

    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void *pixels, RenderUsage usage = kRenderUsageImmutable)
    {
        EPI_UNUSED(usage);

        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    }

    void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void *pixels)
    {
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }

    void PixelStorei(GLenum pname, GLint param)
    {
        glPixelStorei(pname, param);
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
    {
        glReadPixels(x, y, width, height, format, type, pixels);
    }

    void Flush()
    {
        glFlush();
    }

    void InvalidateApplied()
    {
        applied_blend_          = !enable_blend_;
        applied_cull_face_      = !enable_cull_face_;
        applied_depth_test_     = !enable_depth_test_;
        applied_scissor_test_   = !enable_scissor_test_;
        applied_depth_mask_     = !depth_mask_;
        applied_polygon_offset_ = !applied_polygon_offset_;

        applied_depth_func_ = 0;
        applied_cull_mode_  = 0;

        applied_blend_source_      = 0;
        applied_blend_destination_ = 0;

        gl_bound_texture_[0] = 0xFFFFFFFFu;
        gl_bound_texture_[1] = 0xFFFFFFFFu;

        state_dirty_ = true;
    }

    void Reset()
    {
        enable_blend_        = false;
        enable_cull_face_    = false;
        enable_depth_test_   = false;
        enable_alpha_test_   = false;
        enable_fog_          = false;
        enable_scissor_test_ = false;

        depth_mask_     = true;
        depth_function_ = GL_LEQUAL;

        line_width_ = 1.0f;

        active_texture_ = GL_TEXTURE0;

        polygon_offset_factor_ = 0.0f;
        polygon_offset_units_  = 0.0f;

        for (int32_t i = 0; i < 2; i++)
        {
            enable_texture_2d_[i]                = false;
            bind_texture_2d_[i]                  = 0;
            gl_bound_texture_[i]                 = 0;
            texture_environment_mode_[i]         = GL_MODULATE;
            texture_environment_combine_rgb_[i]  = GL_MODULATE;
            texture_environment_source_0_rgb_[i] = GL_TEXTURE;
        }

        for (int32_t i = 0; i < kGles2MaximumClipPlanes; i++)
        {
            clip_plane_enabled_[i] = false;
        }

        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);

        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

        stencil_write_mask_ = 0xFF;

        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);

        glFrontFace(front_face_);

        glClearColor(epi::GetRGBARed(clear_color_) / 255.0f, epi::GetRGBAGreen(clear_color_) / 255.0f,
                     epi::GetRGBABlue(clear_color_) / 255.0f, epi::GetRGBAAlpha(clear_color_) / 255.0f);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        applied_blend_        = false;
        applied_cull_face_    = false;
        applied_depth_test_   = false;
        applied_scissor_test_ = false;
        applied_depth_mask_   = true;
        applied_depth_func_   = GL_LEQUAL;
        applied_cull_mode_    = GL_BACK;
        applied_polygon_offset_ = false;

        applied_blend_source_      = GL_ONE;
        applied_blend_destination_ = GL_ZERO;

        state_dirty_ = true;
    }

    void SetPipeline(uint32_t flags)
    {
        EPI_UNUSED(flags);

        state_dirty_ = true;
    }

    void SetVertexArrays(const RendererVertex *vertices, int count)
    {
        vertex_array_base_ = vertices;

        if (vertices && count > 0)
            gles2_immediate.UploadBatch(vertices, count);
    }

    void DrawVertexArray(GLuint shape, int first, int count)
    {
        if (!vertex_array_base_ || count <= 0)
        {
            return;
        }

        ApplyState();

        gles2_immediate.Draw(shape, vertex_array_base_, first, count);
    }

    uint32_t CreateModelMesh(const ModelMeshData &data, const uint16_t *indices, int index_count)
    {
        return gles2_immediate.CreateModelMesh(data, indices, index_count);
    }

    void DeleteModelMesh(uint32_t handle)
    {
        gles2_immediate.DeleteModelMesh(handle);
    }

    void UpdateModelColors(uint32_t handle, const float *colors, int vertex_count)
    {
        gles2_immediate.UpdateModelColors(handle, colors, vertex_count);
    }

    void DrawModel(const ModelDrawInfo &info)
    {
        if (info.handle == 0 || info.index_count <= 0)
            return;

        ApplyState();

        gles2_model_program.Use();

        const HMM_Mat4 &model_view = gles2_immediate.ModelViewMatrix();
        const HMM_Mat4 &projection = gles2_immediate.ProjectionMatrix();

        gles2_model_program.SetMatrices(HMM_MulM4(projection, model_view), model_view);
        gles2_model_program.SetTransform(info.transform);
        gles2_model_program.SetLerp(info.lerp);
        gles2_model_program.SetTextureTransform(info.texture_scale, info.texture_offset);
        gles2_model_program.SetAlpha(info.alpha);
        gles2_model_program.SetAlphaTest(info.alpha_test);
        gles2_model_program.SetAdditivePass(info.additive_pass);

        Gles2FogMode fog_mode = kGles2FogModeNone;

        if (enable_fog_)
            fog_mode = (fog_mode_ == GL_LINEAR) ? kGles2FogModeLinear : kGles2FogModeExponential;

        gles2_model_program.SetFog(fog_mode, epi::GetRGBARed(fog_color_) / 255.0f,
                                   epi::GetRGBAGreen(fog_color_) / 255.0f, epi::GetRGBABlue(fog_color_) / 255.0f,
                                   fog_density_, fog_start_, fog_end_);

        for (int32_t i = 0; i < kGles2MaximumClipPlanes; i++)
        {
            float equation[4];

            for (int32_t e = 0; e < 4; e++)
                equation[e] = (float)clip_plane_equation_[i][e];

            gles2_model_program.SetClipPlane(i, clip_plane_enabled_[i], equation);
        }

        gles2_immediate.DrawModelMesh(info);

        gles2_program.Use();

        state_dirty_ = true;
    }

    void SetModelIndices(const uint16_t *indices, int count)
    {
        gles2_immediate.UploadModelIndices(indices, count);
    }

    void DrawModelIndexed(int index_first, int index_count)
    {
        if (!vertex_array_base_ || index_count <= 0)
        {
            return;
        }

        ApplyState();

        gles2_immediate.DrawModelIndexed(index_first, index_count);
    }

    void ApplyState()
    {
        if (!state_dirty_)
        {
            return;
        }

        state_dirty_ = false;

        if (applied_blend_ != enable_blend_)
        {
            applied_blend_ = enable_blend_;

            if (enable_blend_)
            {
                glEnable(GL_BLEND);
            }
            else
            {
                glDisable(GL_BLEND);
            }
        }

        if (applied_blend_source_ != blend_source_factor_ || applied_blend_destination_ != blend_destination_factor_)
        {
            applied_blend_source_      = blend_source_factor_;
            applied_blend_destination_ = blend_destination_factor_;

            glBlendFunc(blend_source_factor_, blend_destination_factor_);
        }

        if (applied_cull_face_ != enable_cull_face_)
        {
            applied_cull_face_ = enable_cull_face_;

            if (enable_cull_face_)
            {
                glEnable(GL_CULL_FACE);
            }
            else
            {
                glDisable(GL_CULL_FACE);
            }
        }

        if (applied_cull_mode_ != cull_face_)
        {
            applied_cull_mode_ = cull_face_;

            glCullFace(cull_face_);
        }

        if (applied_depth_test_ != enable_depth_test_)
        {
            applied_depth_test_ = enable_depth_test_;

            if (enable_depth_test_)
            {
                glEnable(GL_DEPTH_TEST);
            }
            else
            {
                glDisable(GL_DEPTH_TEST);
            }
        }

        if (applied_depth_mask_ != depth_mask_)
        {
            applied_depth_mask_ = depth_mask_;

            glDepthMask(depth_mask_ ? GL_TRUE : GL_FALSE);
        }

        if (applied_depth_func_ != depth_function_)
        {
            applied_depth_func_ = depth_function_;

            glDepthFunc(depth_function_);
        }

        if (applied_scissor_test_ != enable_scissor_test_)
        {
            applied_scissor_test_ = enable_scissor_test_;

            if (enable_scissor_test_)
            {
                glEnable(GL_SCISSOR_TEST);
            }
            else
            {
                glDisable(GL_SCISSOR_TEST);
            }
        }

        bool polygon_offset = !epi::AlmostEquals(polygon_offset_factor_, 0.0f) ||
                              !epi::AlmostEquals(polygon_offset_units_, 0.0f);

        if (applied_polygon_offset_ != polygon_offset)
        {
            applied_polygon_offset_ = polygon_offset;

            if (polygon_offset)
            {
                glEnable(GL_POLYGON_OFFSET_FILL);
            }
            else
            {
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
        }

        if (polygon_offset)
        {
            glPolygonOffset(polygon_offset_factor_, polygon_offset_units_);
        }

        bool multi_texture = enable_texture_2d_[1] && bind_texture_2d_[1] != 0;

        for (GLuint i = 0; i < 2; i++)
        {
            GLuint effective = (enable_texture_2d_[i] && bind_texture_2d_[i] != 0) ? bind_texture_2d_[i]
                                                                                  : gles2_immediate.DefaultTexture();

            if (gl_bound_texture_[i] == effective)
            {
                continue;
            }

            gl_bound_texture_[i] = effective;

            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, effective);
        }

        glActiveTexture(active_texture_);

        gles2_program.SetMultiTexture(multi_texture);

        gles2_program.SetSkipRGB(texture_environment_mode_[0] == GL_COMBINE &&
                                 texture_environment_combine_rgb_[0] == GL_REPLACE &&
                                 texture_environment_source_0_rgb_[0] == GL_PREVIOUS);

        gles2_program.SetAlphaTest(enable_alpha_test_ ? alpha_function_reference_ : 0.0f);

        Gles2FogMode fog_mode = kGles2FogModeNone;

        if (enable_fog_)
        {
            fog_mode = (fog_mode_ == GL_LINEAR) ? kGles2FogModeLinear : kGles2FogModeExponential;
        }

        gles2_program.SetFog(fog_mode, epi::GetRGBARed(fog_color_) / 255.0f, epi::GetRGBAGreen(fog_color_) / 255.0f,
                             epi::GetRGBABlue(fog_color_) / 255.0f, fog_density_, fog_start_, fog_end_);

        for (int32_t i = 0; i < kGles2MaximumClipPlanes; i++)
        {
            gles2_program.SetClipPlane(i, clip_plane_equation_[i]);
            gles2_program.SetClipPlaneEnabled(i, clip_plane_enabled_[i]);
        }
    }

  private:
    bool   enable_blend_             = false;
    GLenum blend_source_factor_      = GL_SRC_ALPHA;
    GLenum blend_destination_factor_ = GL_ONE_MINUS_SRC_ALPHA;

    bool   enable_cull_face_ = false;
    GLenum cull_face_        = GL_BACK;
    GLenum front_face_       = GL_CCW;

    bool   enable_depth_test_ = false;
    bool   depth_mask_        = true;
    GLenum depth_function_    = GL_LEQUAL;

    bool enable_scissor_test_ = false;

    GLint   scissor_x_      = 0;
    GLint   scissor_y_      = 0;
    GLsizei scissor_width_  = 0;
    GLsizei scissor_height_ = 0;

    bool     clip_plane_enabled_[kGles2MaximumClipPlanes]     = {};
    GLdouble clip_plane_equation_[kGles2MaximumClipPlanes][4] = {};

    bool    enable_alpha_test_        = false;
    GLenum  alpha_function_           = GL_GREATER;
    GLfloat alpha_function_reference_ = 0.0f;

    GLfloat polygon_offset_factor_ = 0.0f;
    GLfloat polygon_offset_units_  = 0.0f;

    bool enable_texture_2d_[2] = {false, false};

    GLint texture_environment_mode_[2]         = {GL_MODULATE, GL_MODULATE};
    GLint texture_environment_combine_rgb_[2]  = {GL_MODULATE, GL_MODULATE};
    GLint texture_environment_source_0_rgb_[2] = {GL_TEXTURE, GL_TEXTURE};

    GLuint bind_texture_2d_[2] = {0, 0};
    GLuint gl_bound_texture_[2] = {0, 0};
    GLenum active_texture_      = GL_TEXTURE0;

    bool      enable_fog_  = false;
    GLint     fog_mode_    = GL_EXP;
    GLfloat   fog_start_   = 0.0f;
    GLfloat   fog_end_     = 0.0f;
    GLfloat   fog_density_ = 0.0f;
    RGBAColor fog_color_   = kRGBABlack;
    RGBAColor clear_color_ = kRGBABlack;

    float line_width_ = 1.0f;

    bool   applied_blend_             = false;
    bool   applied_cull_face_         = false;
    bool   applied_depth_test_        = false;
    bool   applied_scissor_test_      = false;
    bool   applied_depth_mask_        = true;
    bool   applied_polygon_offset_    = false;
    GLenum applied_depth_func_        = GL_LEQUAL;
    GLenum applied_cull_mode_         = GL_BACK;
    GLenum applied_blend_source_      = GL_ONE;
    GLenum applied_blend_destination_ = GL_ZERO;

    GLuint stencil_write_mask_ = 0xFF;

    bool state_dirty_ = true;

    const RendererVertex *vertex_array_base_ = nullptr;
};

static Gles2RenderState state;
RenderState            *render_state = &state;

void Gles2ApplyRenderState()
{
    state.ApplyState();
}

void Gles2InvalidateRenderState()
{
    state.InvalidateApplied();
}

#include "epi.h"
#include "gpu_device.h"
#include "r_state.h"
#include "r_units.h"

std::unordered_map<GLuint, GLint> texture_clamp_s;
std::unordered_map<GLuint, GLint> texture_clamp_t;

class GpuRenderState : public RenderState
{
  public:
    void Enable(GLenum cap, bool enabled = true)
    {
        switch (cap)
        {
        case GL_TEXTURE_2D:
            enable_texture_2d_[active_texture_ - GL_TEXTURE0] = enabled;
            break;
        case GL_FOG:
            enable_fog_ = enabled;
            break;
        case GL_ALPHA_TEST:
            enable_alpha_test_ = enabled;
            break;
        case GL_BLEND:
            enable_blend_ = enabled;
            break;
        case GL_CULL_FACE:
            enable_cull_face_ = enabled;
            break;
        case GL_SCISSOR_TEST:
            enable_scissor_test_ = enabled;
            break;
        case GL_DEPTH_TEST:
            enable_depth_test_ = enabled;
            break;
        case GL_CLIP_PLANE0:
        case GL_CLIP_PLANE1:
        case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3:
        case GL_CLIP_PLANE4:
        case GL_CLIP_PLANE5:
            enable_clip_plane_[cap - GL_CLIP_PLANE0] = enabled;
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
        depth_mask_ = enable;
    }

    void DepthFunction(GLenum func)
    {
        depth_function_ = func;
    }

    void ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
    {
        EPI_UNUSED(red);
        EPI_UNUSED(green);
        EPI_UNUSED(blue);
        EPI_UNUSED(alpha);
    }

    void CullFace(GLenum mode)
    {
        cull_face_ = mode;
    }

    void AlphaFunction(GLenum func, GLfloat ref)
    {
        alpha_function_           = func;
        alpha_function_reference_ = ref;
    }

    void ActiveTexture(GLenum activeTexture)
    {
        active_texture_ = activeTexture;
    }

    void BindTexture(GLuint textureid)
    {
        bind_texture_2d_[active_texture_ - GL_TEXTURE0] = textureid;
    }

    void ClipPlane(GLenum plane, GLdouble *equation)
    {
        int index = plane - GL_CLIP_PLANE0;

        for (int i = 0; i < 4; i++)
            clip_plane_equation_[index][i] = (float)equation[i];
    }

    void PolygonOffset(GLfloat factor, GLfloat units)
    {
        polygon_offset_factor_ = factor;
        polygon_offset_units_  = units;
    }

    void Clear(GLbitfield mask)
    {
        EPI_UNUSED(mask);
    }

    void ClearColor(RGBAColor color)
    {
        clear_color_ = color;
    }

    void FogMode(GLint fogMode)
    {
        fog_mode_ = fogMode;
    }

    void FogColor(RGBAColor color)
    {
        fog_color_ = color;
    }

    void FogStart(GLfloat start)
    {
        fog_start_ = start;
    }

    void FogEnd(GLfloat end)
    {
        fog_end_ = end;
    }

    void FogDensity(GLfloat density)
    {
        fog_density_ = density;
    }

    void GLColor(RGBAColor color)
    {
        gl_color_ = color;
    }

    void BlendFunction(GLenum sfactor, GLenum dfactor)
    {
        blend_source_factor_      = sfactor;
        blend_destination_factor_ = dfactor;
    }

    void TextureEnvironmentMode(GLint param)
    {
        texture_environment_mode_[active_texture_ - GL_TEXTURE0] = param;
    }

    void TextureEnvironmentCombineRGB(GLint param)
    {
        texture_environment_combine_rgb_[active_texture_ - GL_TEXTURE0] = param;
    }

    void TextureEnvironmentSource0RGB(GLint param)
    {
        texture_environment_source_0_rgb_[active_texture_ - GL_TEXTURE0] = param;
    }

    void TextureMinFilter(GLint param)
    {
        EPI_UNUSED(param);
    }

    void TextureMagFilter(GLint param)
    {
        EPI_UNUSED(param);
    }

    void TextureWrapS(GLint param)
    {
        EPI_UNUSED(param);
    }

    void TextureWrapT(GLint param)
    {
        EPI_UNUSED(param);
    }

    void MultiTexCoord(GLuint tex, const HMM_Vec2 *coords)
    {
        EPI_UNUSED(tex);
        EPI_UNUSED(coords);
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
        }
    }

    void FrontFace(GLenum wind)
    {
        front_face_ = wind;
    }

    void ShadeModel(GLenum model)
    {
        EPI_UNUSED(model);
    }

    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        EPI_UNUSED(x);
        EPI_UNUSED(y);
        EPI_UNUSED(width);
        EPI_UNUSED(height);
    }

    void GenTextures(GLsizei n, GLuint *textures)
    {
        for (GLsizei i = 0; i < n; i++)
            textures[i] = next_texture_id_++;
    }

    void FinishTextures(GLsizei n, GLuint *textures)
    {
        EPI_UNUSED(n);
        EPI_UNUSED(textures);
    }

    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void *pixels, RenderUsage usage = kRenderUsageImmutable)
    {
        EPI_UNUSED(target);
        EPI_UNUSED(level);
        EPI_UNUSED(internalformat);
        EPI_UNUSED(width);
        EPI_UNUSED(height);
        EPI_UNUSED(border);
        EPI_UNUSED(format);
        EPI_UNUSED(type);
        EPI_UNUSED(pixels);
        EPI_UNUSED(usage);
    }

    void PixelStorei(GLenum pname, GLint param)
    {
        EPI_UNUSED(pname);
        EPI_UNUSED(param);
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
    {
        EPI_UNUSED(x);
        EPI_UNUSED(y);
        EPI_UNUSED(format);
        EPI_UNUSED(type);

        if (pixels)
            memset(pixels, 0, (size_t)width * (size_t)height * 4);
    }

    void PixelZoom(GLfloat xfactor, GLfloat yfactor)
    {
        EPI_UNUSED(xfactor);
        EPI_UNUSED(yfactor);
    }

    void Flush()
    {
    }

    void OnContextSwitch()
    {
    }

    void Reset()
    {
        enable_blend_        = false;
        enable_cull_face_    = false;
        enable_depth_test_   = false;
        enable_scissor_test_ = false;
        enable_alpha_test_   = false;
        enable_fog_          = false;

        depth_mask_ = true;

        active_texture_ = GL_TEXTURE0;

        for (int i = 0; i < 2; i++)
        {
            enable_texture_2d_[i] = false;
            bind_texture_2d_[i]   = 0;
        }
    }

    void SetPipeline(uint32_t flags)
    {
        pipeline_flags_ = flags;
    }

    void SetVertexArrays(const RendererVertex *vertices)
    {
        vertex_array_base_ = vertices;
    }

    void DrawVertexArray(GLuint shape, int first, int count)
    {
        EPI_UNUSED(shape);
        EPI_UNUSED(first);
        EPI_UNUSED(count);
    }

  private:
    bool   enable_blend_             = false;
    GLenum blend_source_factor_      = GL_SRC_ALPHA;
    GLenum blend_destination_factor_ = GL_ONE_MINUS_SRC_ALPHA;

    bool   enable_cull_face_ = false;
    GLenum cull_face_        = GL_BACK;
    GLenum front_face_       = GL_CW;

    bool enable_scissor_test_ = false;
    bool enable_depth_test_   = false;
    bool depth_mask_          = true;

    GLenum depth_function_ = GL_LEQUAL;

    bool  enable_clip_plane_[6]      = {false, false, false, false, false, false};
    float clip_plane_equation_[6][4] = {};

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
    GLenum active_texture_     = GL_TEXTURE0;

    bool      enable_fog_   = false;
    GLint     fog_mode_     = GL_EXP;
    GLfloat   fog_start_    = 0.0f;
    GLfloat   fog_end_      = 0.0f;
    GLfloat   fog_density_  = 0.0f;
    RGBAColor fog_color_    = kRGBABlack;
    RGBAColor clear_color_  = kRGBABlack;
    RGBAColor gl_color_     = kRGBAWhite;

    float line_width_ = 1.0f;

    uint32_t pipeline_flags_ = 0;

    GLuint next_texture_id_ = 1;

    const RendererVertex *vertex_array_base_ = nullptr;
};

static GpuRenderState state;
RenderState          *render_state = &state;

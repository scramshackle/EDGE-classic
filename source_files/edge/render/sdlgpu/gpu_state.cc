#include <stdlib.h>

#include <vector>

#include "epi.h"
#include "gpu_device.h"
#include "gpu_images.h"
#include "gpu_immediate.h"
#include "gpu_pipeline.h"
#include "i_system.h"
#include "r_backend.h"
#include "r_state.h"
#include "r_units.h"

std::unordered_map<GLuint, GLint> texture_clamp_s;
std::unordered_map<GLuint, GLint> texture_clamp_t;

static constexpr float kGpuSamplerUnclampedLevelOfDetail = 1000.0f;

struct GpuMipLevel
{
    GLsizei width;
    GLsizei height;
    void   *pixels;
};

static SDL_GPUSamplerAddressMode GpuAddressMode(GLint wrap)
{
    if (wrap == GL_CLAMP || wrap == GL_CLAMP_TO_EDGE)
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

static void GpuFillSamplerInfo(SDL_GPUSamplerCreateInfo *info, GLint minification, GLint magnification, GLint wrap_s,
                               GLint wrap_t)
{
    EPI_CLEAR_MEMORY(info, SDL_GPUSamplerCreateInfo, 1);

    info->address_mode_u = GpuAddressMode(wrap_s);
    info->address_mode_v = GpuAddressMode(wrap_t);
    info->address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    info->mag_filter = (magnification == GL_LINEAR) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;

    bool mipmapped = true;

    switch (minification)
    {
    case GL_NEAREST:
        info->min_filter  = SDL_GPU_FILTER_NEAREST;
        info->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        mipmapped         = false;
        break;

    case GL_LINEAR:
        info->min_filter  = SDL_GPU_FILTER_LINEAR;
        info->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        mipmapped         = false;
        break;

    case GL_NEAREST_MIPMAP_NEAREST:
        info->min_filter  = SDL_GPU_FILTER_NEAREST;
        info->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        break;

    case GL_LINEAR_MIPMAP_NEAREST:
        info->min_filter  = SDL_GPU_FILTER_LINEAR;
        info->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        break;

    case GL_NEAREST_MIPMAP_LINEAR:
        info->min_filter  = SDL_GPU_FILTER_NEAREST;
        info->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        break;

    case GL_LINEAR_MIPMAP_LINEAR:
        info->min_filter  = SDL_GPU_FILTER_LINEAR;
        info->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        break;

    default:
        FatalError("GpuRenderState: unknown texture minification filter 0x%04X\n", minification);
    }

    info->min_lod = 0.0f;
    info->max_lod = mipmapped ? kGpuSamplerUnclampedLevelOfDetail : 0.0f;
}

class GpuRenderState : public RenderState
{
  public:
    GpuRenderState()
    {
        Reset();
    }

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
            if (enabled != scissor_.enabled_)
            {
                scissor_.enabled_ = enabled;
                scissor_.dirty_   = true;
            }
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
            if (clip_planes_[cap - GL_CLIP_PLANE0].enabled_ != enabled)
            {
                clip_planes_[cap - GL_CLIP_PLANE0].enabled_ = enabled;
                clip_planes_[cap - GL_CLIP_PLANE0].dirty_   = true;
            }
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
            clip_planes_[index].equation_[i] = equation[i];

        clip_planes_[index].dirty_ = true;
    }

    void PolygonOffset(GLfloat factor, GLfloat units)
    {
        polygon_offset_factor_ = factor;
        polygon_offset_units_  = units;
    }

    void Clear(GLbitfield mask)
    {
        if (mask & GL_DEPTH_BUFFER_BIT)
            gpu_immediate.ClearDepth();
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
        texture_min_filter_ = param;
    }

    void TextureMagFilter(GLint param)
    {
        texture_mag_filter_ = param;
    }

    void TextureWrapS(GLint param)
    {
        texture_wrap_s_ = param;
    }

    void TextureWrapT(GLint param)
    {
        texture_wrap_t_ = param;
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

            DeleteGpuImage(*tex_id);
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
        scissor_.x_      = x;
        scissor_.y_      = y;
        scissor_.width_  = width;
        scissor_.height_ = height;
        scissor_.dirty_  = true;
    }

    void GenTextures(GLsizei n, GLuint *textures)
    {
        if (generating_texture_)
            FatalError("GenTextures: called during texture generation");

        for (GLsizei i = 0; i < n; i++)
            textures[i] = next_texture_id_++;

        generating_texture_ = true;
        generating_level_   = 0;

        texture_wrap_s_ = GL_CLAMP;
        texture_wrap_t_ = GL_CLAMP;

        DiscardMipLevels();
    }

    void FinishTextures(GLsizei n, GLuint *textures)
    {
        EPI_UNUSED(n);

        if (!generating_texture_)
            FatalError("FinishTextures: called outside of texture generation");

        if (mip_levels_.empty())
            FatalError("FinishTextures: no mip levels defined");

        SDL_GPUSamplerCreateInfo sampler_info;

        GpuFillSamplerInfo(&sampler_info, texture_min_filter_, texture_mag_filter_, texture_wrap_s_, texture_wrap_t_);

        std::vector<GpuImageLevel> levels;

        levels.resize(mip_levels_.size());

        for (size_t i = 0; i < mip_levels_.size(); i++)
        {
            levels[i].width  = mip_levels_[i].width;
            levels[i].height = mip_levels_[i].height;
            levels[i].pixels = mip_levels_[i].pixels;
        }

        if (!CreateGpuImage(gpu_device.Handle(), *textures, levels.data(), (int32_t)levels.size(), &sampler_info))
            FatalError("FinishTextures: failed to create texture %u", *textures);

        DiscardMipLevels();

        generating_texture_ = false;
        generating_level_   = 0;
    }

    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void *pixels, RenderUsage usage = kRenderUsageImmutable)
    {
        EPI_UNUSED(target);
        EPI_UNUSED(border);
        EPI_UNUSED(format);
        EPI_UNUSED(type);

        if (internalformat == GL_RGB)
            FatalError("TexImage2D: GL_RGB is only supported by OpenGL, promote to GL_RGBA before calling TexImage2D");

        if (internalformat == GL_ALPHA)
            FatalError("TexImage2D: GL_ALPHA is only supported by OpenGL, promote to GL_RGBA before calling "
                       "TexImage2D");

        if (internalformat != GL_RGBA)
            FatalError("TexImage2D: unknown texture format");

        if (generating_texture_)
        {
            if (level < generating_level_)
                FatalError("TexImage2D: texture levels must be sequential");

            generating_level_ = level;

            GpuMipLevel mip_level;

            mip_level.width  = width;
            mip_level.height = height;
            mip_level.pixels = nullptr;

            if (pixels && usage == kRenderUsageImmutable)
            {
                size_t bytes = (size_t)width * (size_t)height * 4;

                mip_level.pixels = malloc(bytes);

                memcpy(mip_level.pixels, pixels, bytes);
            }

            mip_levels_.push_back(mip_level);
            return;
        }

        GLuint texture_id = bind_texture_2d_[active_texture_ - GL_TEXTURE0];

        if (!texture_id)
            FatalError("TexImage2D: no texture bound on update");

        if (!UpdateGpuImage(gpu_device.Handle(), texture_id, width, height, pixels))
            FatalError("TexImage2D: failed to update texture %u", texture_id);
    }

    void PixelStorei(GLenum pname, GLint param)
    {
        EPI_UNUSED(pname);
        EPI_UNUSED(param);
    }

    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
    {
        if (!pixels || width <= 0 || height <= 0)
            return;

        if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
            FatalError("ReadPixels: unsupported format 0x%04x / type 0x%04x\n", format, type);

        if (!gpu_device.ReadColorRegion(x, y, width, height, width * 4, (uint8_t *)pixels))
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
        enable_alpha_test_   = false;
        enable_fog_          = false;

        depth_mask_     = true;
        depth_function_ = GL_LEQUAL;

        line_width_ = 1.0f;

        active_texture_ = GL_TEXTURE0;

        for (int i = 0; i < 2; i++)
        {
            enable_texture_2d_[i] = false;
            bind_texture_2d_[i]   = 0;
        }

        for (int i = 0; i < kGpuMaximumClipPlanes; i++)
        {
            clip_planes_[i].enabled_ = false;
            clip_planes_[i].dirty_   = false;
        }

        scissor_.enabled_ = false;
        scissor_.dirty_   = false;
    }

    void SetPipeline(uint32_t flags)
    {
        uint32_t pipeline_flags = 0;

        if (depth_mask_)
            pipeline_flags |= kGpuPipelineDepthWrite;

        if (depth_function_ == GL_GREATER)
            pipeline_flags |= kGpuPipelineDepthGreater;

        if (enable_depth_test_)
            pipeline_flags |= kGpuPipelineDepthTest;

        if (enable_blend_)
            pipeline_flags |= kGpuPipelineBlend;

        if (enable_cull_face_)
        {
            if (cull_face_ == GL_BACK)
                pipeline_flags |= kGpuPipelineCullBack;
            else if (cull_face_ == GL_FRONT)
                pipeline_flags |= kGpuPipelineCullFront;
        }

        pipeline_flags |= flags;

        gpu_immediate.SetPipelineState(pipeline_flags, blend_source_factor_, blend_destination_factor_);

        GpuFogMode fog_mode = kGpuFogModeNone;

        if (enable_fog_)
        {
            if (fog_mode_ == GL_LINEAR)
                fog_mode = kGpuFogModeLinear;
            else if (fog_mode_ == GL_EXP)
                fog_mode = kGpuFogModeExponential;
        }

        gpu_immediate.SetFog(fog_mode, epi::GetRGBARed(fog_color_) / 255.0f, epi::GetRGBAGreen(fog_color_) / 255.0f,
                             epi::GetRGBABlue(fog_color_) / 255.0f, 1.0f, fog_density_, fog_start_, fog_end_, 1.0f);

        gpu_immediate.SetAlphaTest(enable_alpha_test_ ? alpha_function_reference_ : 0.0f);

        if (scissor_.dirty_)
        {
            scissor_.dirty_ = false;

            if (scissor_.enabled_)
            {
                gpu_immediate.ScissorRect(scissor_.x_, scissor_.y_, scissor_.width_, scissor_.height_);
            }
            else
            {
                PassInfo pass_info;
                render_backend->GetPassInfo(pass_info);
                gpu_immediate.ScissorRect(0, 0, pass_info.width_, pass_info.height_);
            }
        }

        for (int i = 0; i < kGpuMaximumClipPlanes; i++)
        {
            if (!clip_planes_[i].dirty_)
                continue;

            clip_planes_[i].dirty_ = false;

            gpu_immediate.SetClipPlaneEnabled(i, clip_planes_[i].enabled_);
            gpu_immediate.SetClipPlane(i, clip_planes_[i].equation_);
        }
    }

    void SetVertexArrays(const RendererVertex *vertices)
    {
        vertex_array_base_ = vertices;
    }

    void DrawVertexArray(GLuint shape, int first, int count)
    {
        if (!vertex_array_base_ || count <= 0)
            return;

        if (enable_texture_2d_[0])
        {
            const GpuImage *image0 = GetGpuImage(bind_texture_2d_[0]);

            if (enable_texture_2d_[1])
            {
                const GpuImage *image1 = GetGpuImage(bind_texture_2d_[1]);

                gpu_immediate.SetMultiTexture(image0 ? image0->texture : nullptr, image0 ? image0->sampler : nullptr,
                                              image1 ? image1->texture : nullptr, image1 ? image1->sampler : nullptr);
            }
            else
            {
                gpu_immediate.SetTexture(image0 ? image0->texture : nullptr, image0 ? image0->sampler : nullptr);
            }
        }
        else
        {
            gpu_immediate.DisableTexture();
        }

        gpu_immediate.Draw(shape, vertex_array_base_ + first, count);
    }

  private:
    void DiscardMipLevels()
    {
        for (size_t i = 0; i < mip_levels_.size(); i++)
        {
            if (mip_levels_[i].pixels)
                free(mip_levels_[i].pixels);
        }

        mip_levels_.clear();
    }

    struct GpuClipPlane
    {
        bool     enabled_;
        bool     dirty_;
        GLdouble equation_[4];
    };

    struct GpuScissor
    {
        bool    enabled_;
        bool    dirty_;
        GLint   x_;
        GLint   y_;
        GLsizei width_;
        GLsizei height_;
    };

    bool   enable_blend_             = false;
    GLenum blend_source_factor_      = GL_SRC_ALPHA;
    GLenum blend_destination_factor_ = GL_ONE_MINUS_SRC_ALPHA;

    bool   enable_cull_face_ = false;
    GLenum cull_face_        = GL_BACK;
    GLenum front_face_       = GL_CW;

    bool enable_depth_test_ = false;
    bool depth_mask_        = true;

    GLenum depth_function_ = GL_LEQUAL;

    GpuClipPlane clip_planes_[kGpuMaximumClipPlanes] = {};

    GpuScissor scissor_ = {};

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

    GLint texture_min_filter_ = GL_NEAREST;
    GLint texture_mag_filter_ = GL_NEAREST;
    GLint texture_wrap_s_     = GL_CLAMP;
    GLint texture_wrap_t_     = GL_CLAMP;

    bool      enable_fog_  = false;
    GLint     fog_mode_    = GL_EXP;
    GLfloat   fog_start_   = 0.0f;
    GLfloat   fog_end_     = 0.0f;
    GLfloat   fog_density_ = 0.0f;
    RGBAColor fog_color_   = kRGBABlack;
    RGBAColor clear_color_ = kRGBABlack;
    RGBAColor gl_color_    = kRGBAWhite;

    float line_width_ = 1.0f;

    GLuint next_texture_id_ = 1;

    bool  generating_texture_ = false;
    GLint generating_level_   = 0;

    std::vector<GpuMipLevel> mip_levels_;

    const RendererVertex *vertex_array_base_ = nullptr;
};

static GpuRenderState state;
RenderState          *render_state = &state;

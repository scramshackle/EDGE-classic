#include <stdlib.h>

#include <vector>

#include "epi.h"
#include "gpu_device.h"
#include "gpu_images.h"
#include "gpu_immediate.h"
#include "gpu_lights.h"

#include "r_lightgrid.h"
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
        case GL_STENCIL_TEST:
            enable_stencil_test_ = enabled;
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
        color_write_enabled_ = (red || green || blue || alpha);
    }

    void StencilFunction(GLenum func, GLint ref, GLuint mask)
    {
        EPI_UNUSED(mask);

        stencil_function_  = func;
        stencil_reference_ = (uint8_t)ref;
    }

    void StencilOperation(GLenum stencil_fail, GLenum depth_fail, GLenum depth_pass)
    {
        EPI_UNUSED(stencil_fail);
        EPI_UNUSED(depth_fail);

        stencil_pass_operation_ = depth_pass;
    }

    void StencilWriteMask(GLuint mask)
    {
        stencil_write_mask_ = mask;
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

    void PolygonOffset(GLfloat factor, GLfloat units)
    {
        polygon_offset_factor_ = factor;
        polygon_offset_units_  = units;
    }

    void Clear(GLbitfield mask)
    {
        if (mask & GL_DEPTH_BUFFER_BIT)
            gpu_immediate.ClearDepth();

        if (mask & GL_STENCIL_BUFFER_BIT)
            gpu_immediate.ClearStencil();
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

    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        scissor_.x_      = x;
        scissor_.y_      = y;
        scissor_.width_  = width;
        scissor_.height_ = height;
        scissor_.dirty_  = true;
    }

    bool ScissorTestEnabled()
    {
        return scissor_.enabled_;
    }

    void GetScissor(GLint &x, GLint &y, GLsizei &width, GLsizei &height)
    {
        x      = scissor_.x_;
        y      = scissor_.y_;
        width  = scissor_.width_;
        height = scissor_.height_;
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

    void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void *pixels)
    {
        EPI_UNUSED(target);
        EPI_UNUSED(format);
        EPI_UNUSED(type);

        if (level != 0 || xoffset != 0 || yoffset != 0)
            FatalError("TexSubImage2D: only full-image updates of level 0 are supported");

        GLuint texture_id = bind_texture_2d_[active_texture_ - GL_TEXTURE0];

        if (!texture_id)
            FatalError("TexSubImage2D: no texture bound on update");

        if (!UpdateGpuImage(gpu_device.Handle(), texture_id, width, height, pixels))
            FatalError("TexSubImage2D: failed to update texture %u", texture_id);
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

    void Flush()
    {
    }

    void Reset()
    {
        enable_blend_        = false;
        enable_cull_face_    = false;
        enable_depth_test_   = false;
        enable_alpha_test_   = false;
        enable_fog_          = false;
        enable_stencil_test_ = false;

        color_write_enabled_ = true;

        depth_mask_     = true;
        depth_function_ = GL_LEQUAL;

        stencil_function_       = GL_ALWAYS;
        stencil_pass_operation_ = GL_KEEP;
        stencil_reference_      = 0;
        stencil_write_mask_     = 0xFF;

        line_width_ = 1.0f;

        active_texture_ = GL_TEXTURE0;

        for (int i = 0; i < 2; i++)
        {
            enable_texture_2d_[i] = false;
            bind_texture_2d_[i]   = 0;
        }
    }

    void SetPipeline(uint32_t flags)
    {
        uint32_t pipeline_flags = 0;

        if (depth_mask_)
            pipeline_flags |= kGpuPipelineDepthWrite;

        if (depth_function_ == GL_GREATER)
            pipeline_flags |= kGpuPipelineDepthGreater;

        if (enable_depth_test_ && depth_function_ != GL_ALWAYS)
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

        if (enable_stencil_test_)
        {
            if (stencil_function_ == GL_EQUAL)
                pipeline_flags |= kGpuPipelineStencilTest;

            if (stencil_write_mask_ != 0)
            {
                if (stencil_pass_operation_ == GL_REPLACE)
                    pipeline_flags |= kGpuPipelineStencilWrite;
                else if (stencil_pass_operation_ == GL_INCR)
                    pipeline_flags |= kGpuPipelineStencilIncrement;
                else if (stencil_pass_operation_ == GL_DECR)
                    pipeline_flags |= kGpuPipelineStencilDecrement;
            }
        }

        if (!color_write_enabled_)
            pipeline_flags |= kGpuPipelineNoColorWrite;

        pipeline_flags |= flags;

        gpu_immediate.SetStencilReference(stencil_reference_);

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
    }

    void SetVertexArrays(const RendererVertex *vertices, int count)
    {
        vertex_array_base_  = vertices;
        vertex_array_count_ = count;
    }

    void DrawVertexArray(GLuint shape, int first, int count)
    {
        if (!vertex_array_base_ || count <= 0)
            return;

        ApplyTextureBindings();

        gpu_immediate.Draw(shape, vertex_array_base_ + first, count);
    }

    uint32_t CreateModelMesh(const ModelMeshData &data, const uint16_t *indices, int index_count)
    {
        return gpu_immediate.CreateModelMesh(data, indices, index_count);
    }

    void DeleteModelMesh(uint32_t handle)
    {
        gpu_immediate.DeleteModelMesh(handle);
    }

    void UpdateModelColors(uint32_t handle, const float *colors, int vertex_count)
    {
        gpu_immediate.UpdateModelColors(handle, colors, vertex_count);
    }

    void DrawModel(const ModelDrawInfo &info)
    {
        if (info.handle == 0 || info.index_count <= 0)
            return;

        int32_t oit_mode = render_backend->OitMode();

        if (oit_mode != kOitPassNone)
        {
            OitPass model_pass = kOitPassMasked;

            if (info.additive_pass)
                model_pass = kOitPassAdditive;
            else if (info.alpha < 1.0f)
                model_pass = kOitPassAccumulate;

            if (model_pass != oit_mode)
                return;
        }

        if (oit_mode == kOitPassAccumulate)
        {
            Enable(GL_BLEND);
            BlendFunction(GL_ONE, GL_ONE);
            DepthMask(false);

            SetPipeline(0);
        }

        ApplyTextureBindings();

        GpuModelVertexParameters vertex_parameters;
        EPI_CLEAR_MEMORY(&vertex_parameters, GpuModelVertexParameters, 1);

        const HMM_Mat4 &model_view = gpu_immediate.ModelViewMatrix();
        const HMM_Mat4 &projection = gpu_immediate.ProjectionMatrix();

        vertex_parameters.mvp             = HMM_MulM4(projection, model_view);
        vertex_parameters.mv              = model_view;
        vertex_parameters.model_transform = info.transform;
        vertex_parameters.lerp            = info.lerp;

        vertex_parameters.texture_scale[0] = info.texture_scale.X;
        vertex_parameters.texture_scale[1] = info.texture_scale.Y;

        vertex_parameters.texture_offset[0] = info.texture_offset.X;
        vertex_parameters.texture_offset[1] = info.texture_offset.Y;

        GpuModelFragmentParameters fragment_parameters;
        EPI_CLEAR_MEMORY(&fragment_parameters, GpuModelFragmentParameters, 1);

        fragment_parameters.alpha         = info.alpha;
        fragment_parameters.alpha_test    = info.alpha_test;
        fragment_parameters.additive_pass = info.additive_pass ? 1.0f : 0.0f;

        if (enable_fog_)
        {
            fragment_parameters.fog_mode =
                (fog_mode_ == GL_LINEAR) ? (int32_t)kGpuFogModeLinear : (int32_t)kGpuFogModeExponential;

            fragment_parameters.fog_color[0] = epi::GetRGBARed(fog_color_) / 255.0f;
            fragment_parameters.fog_color[1] = epi::GetRGBAGreen(fog_color_) / 255.0f;
            fragment_parameters.fog_color[2] = epi::GetRGBABlue(fog_color_) / 255.0f;
            fragment_parameters.fog_color[3] = 1.0f;

            fragment_parameters.fog_density = fog_density_;
            fragment_parameters.fog_start   = fog_start_;
            fragment_parameters.fog_end     = fog_end_;
        }

        int light_view_index = info.world_lit ? GpuCurrentLightView() : -1;

        const GpuLightViewParameters *light_view = GpuLightView(light_view_index);

        if (light_view)
        {
            for (int i = 0; i < 4; i++)
            {
                fragment_parameters.light_view[i]  = light_view->light_view[i];
                fragment_parameters.light_range[i] = light_view->light_range[i];
            }
        }

        const LightGridGlowSet *glow_set = info.world_lit ? LightGridGlowSetAt(info.glow_set) : nullptr;

        fragment_parameters.glow_additive[3] = (glow_set && glow_set->count > 0) ? (float)glow_set->count : 0.0f;

        for (int i = 0; i < 2; i++)
        {
            bool live = glow_set && i < glow_set->count;

            for (int e = 0; e < 4; e++)
                fragment_parameters.glow_plane[i][e] = live ? glow_set->glows[i].plane[e] : 0.0f;

            fragment_parameters.glow_color[i][0] = live ? glow_set->glows[i].color[0] / 255.0f : 0.0f;
            fragment_parameters.glow_color[i][1] = live ? glow_set->glows[i].color[1] / 255.0f : 0.0f;
            fragment_parameters.glow_color[i][2] = live ? glow_set->glows[i].color[2] / 255.0f : 0.0f;
            fragment_parameters.glow_color[i][3] = live ? glow_set->glows[i].radius : 1.0f;

            fragment_parameters.glow_additive[i] = live ? glow_set->glows[i].additive : 0.0f;
        }

        gpu_immediate.RecordModelDraw(info, vertex_parameters, fragment_parameters);
    }

    void SetModelIndices(const uint16_t *indices, int count)
    {
        model_indices_     = indices;
        model_index_count_ = count;
    }

    void DrawModelIndexed(int index_first, int index_count)
    {
        if (!vertex_array_base_ || !model_indices_ || index_count <= 0)
            return;

        if (index_first < 0 || index_first + index_count > model_index_count_)
            return;

        ApplyTextureBindings();

        gpu_immediate.DrawIndexed(vertex_array_base_, vertex_array_count_, model_indices_ + index_first, index_count);
    }

  private:
    void ApplyTextureBindings()
    {
        if (!enable_texture_2d_[0])
        {
            gpu_immediate.DisableTexture();
            return;
        }

        const GpuImage *image0 = GetGpuImage(bind_texture_2d_[0]);

        if (enable_texture_2d_[1])
        {
            const GpuImage *image1 = GetGpuImage(bind_texture_2d_[1]);

            gpu_immediate.SetMultiTexture(image0 ? image0->texture : nullptr, image0 ? image0->sampler : nullptr,
                                          image1 ? image1->texture : nullptr, image1 ? image1->sampler : nullptr);
            return;
        }

        gpu_immediate.SetTexture(image0 ? image0->texture : nullptr, image0 ? image0->sampler : nullptr);
    }

    void DiscardMipLevels()
    {
        for (size_t i = 0; i < mip_levels_.size(); i++)
        {
            if (mip_levels_[i].pixels)
                free(mip_levels_[i].pixels);
        }

        mip_levels_.clear();
    }

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

    bool enable_depth_test_   = false;
    bool enable_stencil_test_ = false;

    bool color_write_enabled_ = true;

    GLenum  stencil_function_       = GL_ALWAYS;
    GLenum  stencil_pass_operation_ = GL_KEEP;
    uint8_t stencil_reference_      = 0;
    GLuint  stencil_write_mask_     = 0xFF;
    bool depth_mask_        = true;

    GLenum depth_function_ = GL_LEQUAL;


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

    float line_width_ = 1.0f;

    GLuint next_texture_id_ = 1;

    bool  generating_texture_ = false;
    GLint generating_level_   = 0;

    std::vector<GpuMipLevel> mip_levels_;

    const RendererVertex *vertex_array_base_  = nullptr;
    int                   vertex_array_count_ = 0;

    const uint16_t *model_indices_     = nullptr;
    int             model_index_count_ = 0;
};

static GpuRenderState state;
RenderState          *render_state = &state;

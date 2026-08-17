#pragma once

#include <SDL3/SDL.h>
#include <stdint.h>

#include <vector>

#include "HandmadeMath.h"
#include "gpu_pipeline.h"
#include "gpu_shaders.h"
#include "i_defs_gl.h"
#include "r_units.h"

constexpr int32_t kGpuMatrixStackDepth = 32;

constexpr int32_t kGpuMaximumQuads       = 16384;
constexpr int32_t kGpuMaximumFanVertices = 4096;

enum GpuMatrixMode
{
    kGpuMatrixModeModelView = 0,
    kGpuMatrixModeProjection,
    kGpuMatrixModeTexture,
    kGpuMatrixModeTotal
};

enum GpuIndexSource
{
    kGpuIndexSourceNone = 0,
    kGpuIndexSourceQuad,
    kGpuIndexSourceFan,
    kGpuIndexSourceDynamic
};

enum GpuCommandType
{
    kGpuCommandDraw = 0,
    kGpuCommandModelDraw,
    kGpuCommandLightDraw,
    kGpuCommandMovie,
    kGpuCommandViewport,
    kGpuCommandScissor,
    kGpuCommandClearDepth,
    kGpuCommandClearStencil,
    kGpuCommandBeginWorldTarget,
    kGpuCommandResolveWorldTarget
};

struct GpuResolveArguments
{
    int32_t source_x;
    int32_t source_y;
    int32_t source_width;
    int32_t source_height;

    int32_t destination_x;
    int32_t destination_y;
    int32_t destination_width;
    int32_t destination_height;

    bool smooth;
};

struct GpuDrawArguments
{
    SDL_GPUGraphicsPipeline *pipeline;

    SDL_GPUTexture *texture[2];
    SDL_GPUSampler *sampler[2];

    int32_t base_vertex;
    int32_t vertex_count;
    int32_t index_count;
    int32_t index_first;

    int32_t vertex_parameter_index;
    int32_t fragment_parameter_index;

    GpuIndexSource index_source;

    uint8_t stencil_reference;

    bool mergeable;

    SDL_GPUBuffer *vertex_buffer;
};

struct GpuModelDrawArguments
{
    SDL_GPUGraphicsPipeline *pipeline;

    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;

    SDL_GPUBuffer *position_buffer;
    SDL_GPUBuffer *texture_coordinate_buffer;
    SDL_GPUBuffer *color_buffer;
    SDL_GPUBuffer *index_buffer;

    uint32_t position_frame1_offset;
    uint32_t position_frame2_offset;
    uint32_t texture_coordinate_offset;
    uint32_t color_offset;

    int32_t index_first;
    int32_t index_count;

    int32_t vertex_parameter_index;
    int32_t fragment_parameter_index;

    uint8_t stencil_reference;
};

struct GpuLightDrawArguments
{
    SDL_GPUGraphicsPipeline *pipeline;

    SDL_GPUTexture *texture[2];
    SDL_GPUSampler *sampler[2];

    int32_t base_vertex;
    int32_t index_count;
    int32_t index_first;

    int32_t vertex_parameter_index;
    int32_t fragment_parameter_index;

    uint8_t stencil_reference;
};

struct GpuMovieArguments
{
    SDL_GPUTexture *texture[3];
    SDL_GPUSampler *sampler;

    HMM_Mat4 mvp;

    int32_t base_vertex;

    float plane_scales[4];
};

struct GpuRectangleArguments
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct GpuCommand
{
    GpuCommandType type;

    union {
        GpuDrawArguments      draw;
        GpuModelDrawArguments model_draw;
        GpuLightDrawArguments light_draw;
        GpuMovieArguments     movie;
        GpuRectangleArguments rectangle;
        GpuResolveArguments   resolve;
    } arguments;
};

class GpuImmediate
{
  public:
    bool Init(SDL_GPUDevice *device);

    void Shutdown(SDL_GPUDevice *device);

    void BeginFrame();

    void Replay();

    void MatrixModeModelView()
    {
        current_matrix_mode_ = kGpuMatrixModeModelView;
    }

    void MatrixModeProjection()
    {
        current_matrix_mode_ = kGpuMatrixModeProjection;
    }

    void MatrixModeTexture()
    {
        current_matrix_mode_ = kGpuMatrixModeTexture;
    }

    void LoadIdentity();

    void PushMatrix();

    void PopMatrix();

    void LoadMatrix(const HMM_Mat4 &matrix);

    void MultiplyMatrix(const HMM_Mat4 &matrix);

    void Translate(float x, float y, float z);

    void Rotate(float radians, float x, float y, float z);

    void Scale(float x, float y, float z);

    void Orthographic(float left, float right, float bottom, float top, float z_near, float z_far);

    void Frustum(float left, float right, float bottom, float top, float z_near, float z_far);

    const HMM_Mat4 &ModelViewMatrix() const
    {
        return matrix_stack_[kGpuMatrixModeModelView][matrix_top_[kGpuMatrixModeModelView]];
    }

    const HMM_Mat4 &ProjectionMatrix() const
    {
        return matrix_stack_[kGpuMatrixModeProjection][matrix_top_[kGpuMatrixModeProjection]];
    }

    void SetPipelineState(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend);

    void SetStencilReference(uint8_t reference);

    void SetTexture(SDL_GPUTexture *texture, SDL_GPUSampler *sampler);

    void SetMultiTexture(SDL_GPUTexture *texture0, SDL_GPUSampler *sampler0, SDL_GPUTexture *texture1,
                         SDL_GPUSampler *sampler1);

    void DisableTexture();

    void SetAlphaTest(float alpha_test);

    void SetFog(GpuFogMode mode, float red, float green, float blue, float alpha, float density, float start, float end,
                float scale);

    void SetLineMode(bool enabled);

    void SetSkipRGB(bool enabled);

    void SetSkyPass(const SkyPassInfo *sky_pass);

    void SetLightDepth(bool enabled);

    uint32_t CreateStaticBuffer(const RendererVertex *vertices, int count);
    void     DeleteStaticBuffer(uint32_t handle);
    void     DrawStatic(uint32_t handle, int32_t first, int32_t count);

    void SetClipPlane(int32_t index, const double equation[4]);

    void SetClipPlaneEnabled(int32_t index, bool enabled);

    void Viewport(int32_t x, int32_t y, int32_t width, int32_t height);

    void ScissorRect(int32_t x, int32_t y, int32_t width, int32_t height);

    void BeginWorldTarget();

    void ResolveWorldTarget(const GpuResolveArguments &resolve);

    void ClearDepth();

    void ClearStencil();

    RendererVertex *ReserveVertices(int32_t count);

    void RecordDraw(GLuint shape, int32_t count);

    void RecordMovieDraw(SDL_GPUTexture *luma, SDL_GPUTexture *chroma_blue, SDL_GPUTexture *chroma_red,
                         SDL_GPUSampler *sampler, const float plane_scales[4]);

    void Draw(GLuint shape, const RendererVertex *vertices, int32_t count);

    void DrawIndexed(const RendererVertex *vertices, int32_t vertex_count, const uint16_t *indices,
                     int32_t index_count);

    uint32_t CreateModelMesh(const ModelMeshData &data, const uint16_t *indices, int32_t index_count);

    void DeleteModelMesh(uint32_t handle);

    void UpdateModelColors(uint32_t handle, const float *colors, int32_t vertex_count);

    void RecordModelDraw(const ModelDrawInfo &info, const GpuModelVertexParameters &vertex_parameters,
                         const GpuModelFragmentParameters &fragment_parameters);

    void RecordLightDraw(GLuint shape, const RendererVertex *vertices, int32_t count,
                         const GpuLightVertexParameters   &vertex_parameters,
                         const GpuLightFragmentParameters &fragment_parameters);

    uint32_t DrawCount() const
    {
        return draw_count_;
    }

    uint32_t PipelineBindCount() const
    {
        return pipeline_bind_count_;
    }

    uint32_t BindingCount() const
    {
        return binding_count_;
    }

    uint32_t UniformPushCount() const
    {
        return uniform_push_count_;
    }

    size_t UniformBytes() const
    {
        return uniform_bytes_;
    }

    size_t UploadedBytes() const
    {
        return uploaded_bytes_;
    }

  private:
    bool CreateIndexBuffers(SDL_GPUDevice *device);

    bool EnsureVertexCapacity(size_t bytes);

    bool EnsureIndexCapacity(size_t bytes);

    void UploadVertices();

    void UploadIndices();

    int32_t AppendDynamicIndices(GLuint shape, int32_t count, int32_t rebase);

    void MarkMatrixDirty()
    {
        vertex_parameters_dirty_ = true;
    }

    int32_t CurrentVertexParameters();

    int32_t CurrentFragmentParameters();

    void ResetTargetRectangles();

    void ApplyPassState();

    SDL_GPUDevice *device_ = nullptr;

    SDL_GPUBuffer         *vertex_buffer_          = nullptr;
    SDL_GPUTransferBuffer *vertex_transfer_buffer_ = nullptr;
    size_t                 vertex_buffer_capacity_ = 0;

    SDL_GPUBuffer *quad_index_buffer_ = nullptr;
    SDL_GPUBuffer *fan_index_buffer_  = nullptr;

    SDL_GPUBuffer         *dynamic_index_buffer_          = nullptr;
    SDL_GPUTransferBuffer *dynamic_index_transfer_buffer_ = nullptr;
    size_t                 dynamic_index_capacity_        = 0;

    std::vector<uint16_t> dynamic_indices_;

    SDL_GPUTexture *default_texture_ = nullptr;
    SDL_GPUSampler *default_sampler_ = nullptr;

    struct GpuModelMesh
    {
        SDL_GPUBuffer *position_buffer;
        SDL_GPUBuffer *texture_coordinate_buffer;
        SDL_GPUBuffer *color_buffer;
        SDL_GPUBuffer *index_buffer;

        SDL_GPUTransferBuffer *color_transfer_buffer;

        int32_t vertex_count;
        int32_t frame_count;
    };

    std::vector<GpuModelMesh> model_meshes_;

    std::vector<GpuModelVertexParameters>   model_vertex_parameters_;
    std::vector<GpuModelFragmentParameters> model_fragment_parameters_;

    std::vector<GpuLightVertexParameters>   light_vertex_parameters_;
    std::vector<GpuLightFragmentParameters> light_fragment_parameters_;

    std::vector<RendererVertex>        vertices_;
    int32_t                            vertex_count_ = 0;
    std::vector<GpuCommand>            commands_;
    std::vector<GpuVertexParameters>   vertex_parameters_;
    std::vector<GpuFragmentParameters> fragment_parameters_;

    HMM_Mat4 matrix_stack_[kGpuMatrixModeTotal][kGpuMatrixStackDepth];
    int32_t  matrix_top_[kGpuMatrixModeTotal];

    GpuMatrixMode current_matrix_mode_ = kGpuMatrixModeModelView;

    float clip_plane_[kGpuMaximumClipPlanes][4];

    GpuFragmentParameters current_fragment_parameters_;

    bool vertex_parameters_dirty_   = true;
    bool fragment_parameters_dirty_ = true;

    int32_t vertex_parameter_index_   = -1;
    int32_t fragment_parameter_index_ = -1;

    uint32_t pipeline_flags_    = 0;
    uint8_t  stencil_reference_ = 0;
    GLenum   source_blend_      = GL_SRC_ALPHA;
    GLenum   destination_blend_ = GL_ONE_MINUS_SRC_ALPHA;

    SDL_GPUTexture *current_texture_[2] = {nullptr, nullptr};
    SDL_GPUSampler *current_sampler_[2] = {nullptr, nullptr};

    bool texturing_enabled_ = false;

    bool        sky_pass_enabled_ = false;
    bool        light_depth_enabled_ = false;

    std::vector<SDL_GPUBuffer *> static_buffers_;
    SDL_GPUBuffer               *bound_vertex_buffer_ = nullptr;
    SkyPassInfo sky_pass_info_;

    int32_t pending_base_  = 0;
    int32_t pending_count_ = 0;

    SDL_GPUGraphicsPipeline *bound_pipeline_    = nullptr;
    SDL_GPUTexture          *bound_texture_[2]  = {nullptr, nullptr};
    SDL_GPUSampler          *bound_sampler_[2]  = {nullptr, nullptr};
    SDL_GPUBuffer           *bound_index_buffer_ = nullptr;

    int32_t bound_stencil_reference_        = -1;
    int32_t bound_vertex_parameter_index_   = -1;
    int32_t bound_fragment_parameter_index_ = -1;

    GpuRectangleArguments current_viewport_ = {0, 0, 0, 0};
    GpuRectangleArguments current_scissor_  = {0, 0, 0, 0};

    bool viewport_set_ = false;
    bool scissor_set_  = false;

    uint32_t draw_count_          = 0;
    uint32_t pipeline_bind_count_ = 0;
    uint32_t binding_count_       = 0;
    uint32_t uniform_push_count_  = 0;
    size_t   uniform_bytes_       = 0;
    size_t   uploaded_bytes_      = 0;
};

extern GpuImmediate gpu_immediate;

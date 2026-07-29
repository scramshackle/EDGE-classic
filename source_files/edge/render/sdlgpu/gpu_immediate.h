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
    kGpuIndexSourceFan
};

enum GpuCommandType
{
    kGpuCommandDraw = 0,
    kGpuCommandViewport,
    kGpuCommandScissor,
    kGpuCommandClearDepth
};

struct GpuDrawArguments
{
    SDL_GPUGraphicsPipeline *pipeline;

    SDL_GPUTexture *texture[2];
    SDL_GPUSampler *sampler[2];

    int32_t base_vertex;
    int32_t vertex_count;
    int32_t index_count;

    int32_t vertex_parameter_index;
    int32_t fragment_parameter_index;

    GpuIndexSource index_source;

    bool mergeable;
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
        GpuRectangleArguments rectangle;
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

    void SetPipelineState(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend);

    void SetTexture(SDL_GPUTexture *texture, SDL_GPUSampler *sampler);

    void SetMultiTexture(SDL_GPUTexture *texture0, SDL_GPUSampler *sampler0, SDL_GPUTexture *texture1,
                         SDL_GPUSampler *sampler1);

    void DisableTexture();

    void SetAlphaTest(float alpha_test);

    void SetFog(GpuFogMode mode, float red, float green, float blue, float alpha, float density, float start, float end,
                float scale);

    void SetLineMode(bool enabled);

    void SetClipPlane(int32_t index, const double equation[4]);

    void SetClipPlaneEnabled(int32_t index, bool enabled);

    void Viewport(int32_t x, int32_t y, int32_t width, int32_t height);

    void ScissorRect(int32_t x, int32_t y, int32_t width, int32_t height);

    void ClearDepth();

    RendererVertex *ReserveVertices(int32_t count);

    void RecordDraw(GLuint shape, int32_t count);

    void Draw(GLuint shape, const RendererVertex *vertices, int32_t count);

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

    size_t UploadedBytes() const
    {
        return uploaded_bytes_;
    }

  private:
    bool CreateIndexBuffers(SDL_GPUDevice *device);

    bool EnsureVertexCapacity(size_t bytes);

    void UploadVertices();

    void MarkMatrixDirty()
    {
        vertex_parameters_dirty_ = true;
    }

    int32_t CurrentVertexParameters();

    int32_t CurrentFragmentParameters();

    void ApplyPassState();

    SDL_GPUDevice *device_ = nullptr;

    SDL_GPUBuffer         *vertex_buffer_          = nullptr;
    SDL_GPUTransferBuffer *vertex_transfer_buffer_ = nullptr;
    size_t                 vertex_buffer_capacity_ = 0;

    SDL_GPUBuffer *quad_index_buffer_ = nullptr;
    SDL_GPUBuffer *fan_index_buffer_  = nullptr;

    SDL_GPUTexture *default_texture_ = nullptr;
    SDL_GPUSampler *default_sampler_ = nullptr;

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
    GLenum   source_blend_      = GL_SRC_ALPHA;
    GLenum   destination_blend_ = GL_ONE_MINUS_SRC_ALPHA;

    SDL_GPUTexture *current_texture_[2] = {nullptr, nullptr};
    SDL_GPUSampler *current_sampler_[2] = {nullptr, nullptr};

    bool texturing_enabled_ = false;

    int32_t pending_base_  = 0;
    int32_t pending_count_ = 0;

    SDL_GPUGraphicsPipeline *bound_pipeline_    = nullptr;
    SDL_GPUTexture          *bound_texture_[2]  = {nullptr, nullptr};
    SDL_GPUSampler          *bound_sampler_[2]  = {nullptr, nullptr};
    SDL_GPUBuffer           *bound_index_buffer_ = nullptr;

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
    size_t   uploaded_bytes_      = 0;
};

extern GpuImmediate gpu_immediate;

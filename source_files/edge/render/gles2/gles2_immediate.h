#pragma once

#include <stdint.h>

#include <vector>

#include "HandmadeMath.h"
#include "gles2_program.h"
#include "i_defs_gl.h"
#include "r_units.h"

constexpr int32_t kGles2MatrixStackDepth = 32;

constexpr int32_t kGles2MaximumQuads = 16384;

constexpr size_t kGles2VertexBufferBytes = 4 * 1024 * 1024;

struct Gles2ResolveRect
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

enum Gles2MatrixMode
{
    kGles2MatrixModeModelView = 0,
    kGles2MatrixModeProjection,
    kGles2MatrixModeTotal
};

class Gles2Immediate
{
  public:
    bool Init();

    void Shutdown();

    void BeginFrame();

    void MatrixModeModelView()
    {
        current_matrix_mode_ = kGles2MatrixModeModelView;
    }

    void MatrixModeProjection()
    {
        current_matrix_mode_ = kGles2MatrixModeProjection;
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
        return matrix_stack_[kGles2MatrixModeModelView][matrix_top_[kGles2MatrixModeModelView]];
    }

    void ApplyMatrices();

    void Viewport(int32_t x, int32_t y, int32_t width, int32_t height);

    void ClearDepth();

    void SetLineMode(bool enabled);

    void UploadBatch(const RendererVertex *vertices, int32_t count);

    void InvalidateBatch();

    void Draw(GLuint shape, const RendererVertex *vertices, int32_t base, int32_t count);

    void UploadMergedIndices(const uint16_t *indices, int32_t count);

    void DrawMerged(int32_t index_offset, int32_t index_count);

    void DrawMergedWithoutMatrices(int32_t index_offset, int32_t index_count);

    void UploadModelIndices(const uint16_t *indices, int32_t count);

    void DrawModelIndexed(int32_t index_offset, int32_t index_count);

    uint32_t CreateModelMesh(const ModelMeshData &data, const uint16_t *indices, int32_t index_count);

    void DeleteModelMesh(uint32_t handle);

    void UpdateModelColors(uint32_t handle, const float *colors, int32_t vertex_count);

    void BindModelMesh(const ModelDrawInfo &info);

    void DrawModelMesh(const ModelDrawInfo &info);

    void DrawMovieQuad(const RendererVertex *vertices);

    bool EnsureRenderTarget(int32_t width, int32_t height);

    void DestroyRenderTarget();

    void BindRenderTarget();

    void ResolveRenderTarget(const Gles2ResolveRect &source, const Gles2ResolveRect &destination, int32_t window_width,
                             int32_t window_height, bool smooth);

    void BindOitTarget(int32_t mode);

    void ClearOitTargets();

    void CompositeOit(const Gles2ResolveRect &view);

    float OitScale() const
    {
        return oit_scale_;
    }

    bool RenderTargetReady() const
    {
        return render_target_framebuffer_ != 0;
    }

    const HMM_Mat4 &ProjectionMatrix() const
    {
        return matrix_stack_[kGles2MatrixModeProjection][matrix_top_[kGles2MatrixModeProjection]];
    }

    RendererVertex *ReserveVertices(int32_t count);

    void RecordDraw(GLuint shape, int32_t count);

    uint32_t DrawCount() const
    {
        return draw_count_;
    }

    size_t UploadedBytes() const
    {
        return uploaded_bytes_;
    }

    uint32_t UploadCount() const
    {
        return upload_count_;
    }

    GLuint DefaultTexture() const
    {
        return default_texture_;
    }

    void ResetStatistics()
    {
        draw_count_     = 0;
        uploaded_bytes_ = 0;
        upload_count_   = 0;
    }

  private:
    struct Gles2ModelMesh
    {
        GLuint position_buffer;
        GLuint normal_buffer;
        GLuint texture_coordinate_buffer;
        GLuint color_buffer;
        GLuint index_buffer;

        int32_t vertex_count;
        int32_t frame_count;
    };

    std::vector<Gles2ModelMesh> model_meshes_;

    bool CreateQuadIndexBuffer();

    void MarkMatrixDirty()
    {
        matrices_dirty_ = true;
    }

    size_t StreamVertices(const RendererVertex *vertices, int32_t count);

    void BindVertexAttributes(size_t byte_offset);
    void BindVertexAttributesFrom(GLuint buffer);

  public:
    GLuint CreateStaticBuffer(const RendererVertex *vertices, int count);
    void   DeleteStaticBuffer(GLuint buffer);
    void   DrawStatic(GLuint buffer, GLuint shape, int first, int count);

  private:

    void DrawRange(GLuint shape, size_t byte_offset, int32_t count);

    bool CreateDefaultTexture();

    bool AttachRenderTargetDepth(int32_t width, int32_t height);

    void DestroyRenderTargetDepth();

    bool CreateRenderTarget(int32_t width, int32_t height, int32_t texture_width, int32_t texture_height);

    bool CreateOitTargets(int32_t texture_width, int32_t texture_height);

    bool CreateOitTarget(GLuint &framebuffer, GLuint &texture, GLint internal_format, GLenum format, GLenum type,
                         int32_t texture_width, int32_t texture_height);

    void DestroyOitTargets();

    GLuint render_target_framebuffer_ = 0;
    GLuint render_target_color_       = 0;
    GLuint render_target_depth_       = 0;

    int32_t render_target_width_          = 0;
    int32_t render_target_height_         = 0;
    int32_t render_target_texture_width_  = 0;
    int32_t render_target_texture_height_ = 0;

    GLuint render_target_stencil_          = 0;
    GLenum render_target_depth_attachment_ = 0;
    bool   render_target_separate_stencil_ = false;

    GLuint oit_accumulation_framebuffer_ = 0;
    GLuint oit_accumulation_texture_     = 0;
    GLuint oit_revealage_framebuffer_    = 0;
    GLuint oit_revealage_texture_        = 0;

    float oit_scale_ = 1.0f;

    GLuint vertex_buffer_       = 0;
    GLuint quad_index_buffer_   = 0;
    GLuint merged_index_buffer_ = 0;
    GLuint model_index_buffer_  = 0;
    GLuint default_texture_     = 0;

    size_t vertex_buffer_offset_ = 0;

    const RendererVertex *batch_base_   = nullptr;
    int32_t               batch_count_  = 0;
    size_t                batch_offset_ = 0;

    std::vector<RendererVertex> scratch_vertices_;

    HMM_Mat4 matrix_stack_[kGles2MatrixModeTotal][kGles2MatrixStackDepth];
    int32_t  matrix_top_[kGles2MatrixModeTotal];

    Gles2MatrixMode current_matrix_mode_ = kGles2MatrixModeModelView;

    bool matrices_dirty_ = true;

    uint32_t draw_count_     = 0;
    uint32_t upload_count_   = 0;
    size_t   uploaded_bytes_ = 0;
};

extern Gles2Immediate gles2_immediate;

void Gles2ApplyRenderState();

void Gles2InvalidateRenderState();


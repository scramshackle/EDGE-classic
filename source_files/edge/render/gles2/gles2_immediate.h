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

    void DrawMovieQuad(const RendererVertex *vertices);

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
    bool CreateQuadIndexBuffer();

    void MarkMatrixDirty()
    {
        matrices_dirty_ = true;
    }

    size_t StreamVertices(const RendererVertex *vertices, int32_t count);

    void BindVertexAttributes(size_t byte_offset);

    void DrawRange(GLuint shape, size_t byte_offset, int32_t count);

    bool CreateDefaultTexture();

    GLuint vertex_buffer_       = 0;
    GLuint quad_index_buffer_   = 0;
    GLuint merged_index_buffer_ = 0;
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

void Gles2DetectStencilBuffer();

#include "gles2_immediate.h"

#include <stddef.h>
#include <string.h>

#include "epi.h"
#include "i_system.h"

Gles2Immediate gles2_immediate;

static_assert(sizeof(RendererVertex) == 44, "RendererVertex size");
static_assert(offsetof(RendererVertex, rgba) == 0, "RendererVertex::rgba offset");
static_assert(offsetof(RendererVertex, position) == 4, "RendererVertex::position offset");
static_assert(offsetof(RendererVertex, texture_coordinates) == 16, "RendererVertex::texture_coordinates offset");

static HMM_Mat4 Gles2FrustumMatrix(float left, float right, float bottom, float top, float z_near, float z_far)
{
    HMM_Mat4 result = {};

    result.Elements[0][0] = (2.0f * z_near) / (right - left);
    result.Elements[1][1] = (2.0f * z_near) / (top - bottom);

    result.Elements[2][0] = (right + left) / (right - left);
    result.Elements[2][1] = (top + bottom) / (top - bottom);
    result.Elements[2][2] = -z_far / (z_far - z_near);
    result.Elements[2][3] = -1.0f;

    result.Elements[3][2] = -(z_far * z_near) / (z_far - z_near);

    return result;
}

static HMM_Mat4 Gles2OrthographicMatrix(float left, float right, float bottom, float top, float z_near, float z_far)
{
    HMM_Mat4 result = {};

    result.Elements[0][0] = 2.0f / (right - left);
    result.Elements[1][1] = 2.0f / (top - bottom);
    result.Elements[2][2] = -1.0f / (z_far - z_near);
    result.Elements[3][3] = 1.0f;

    result.Elements[3][0] = -(right + left) / (right - left);
    result.Elements[3][1] = -(top + bottom) / (top - bottom);
    result.Elements[3][2] = -z_near / (z_far - z_near);

    return result;
}

bool Gles2Immediate::CreateQuadIndexBuffer()
{
    std::vector<uint16_t> indices;

    indices.resize((size_t)kGles2MaximumQuads * 6);

    for (int32_t quad = 0; quad < kGles2MaximumQuads; quad++)
    {
        uint16_t base = (uint16_t)(quad * 4);

        indices[(size_t)quad * 6 + 0] = base;
        indices[(size_t)quad * 6 + 1] = (uint16_t)(base + 1);
        indices[(size_t)quad * 6 + 2] = (uint16_t)(base + 2);
        indices[(size_t)quad * 6 + 3] = base;
        indices[(size_t)quad * 6 + 4] = (uint16_t)(base + 2);
        indices[(size_t)quad * 6 + 5] = (uint16_t)(base + 3);
    }

    glGenBuffers(1, &quad_index_buffer_);

    if (!quad_index_buffer_)
    {
        return false;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_index_buffer_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indices.size() * sizeof(uint16_t)), indices.data(),
                 GL_STATIC_DRAW);

    return true;
}

bool Gles2Immediate::CreateDefaultTexture()
{
    const uint8_t white[4] = {255, 255, 255, 255};

    glGenTextures(1, &default_texture_);

    if (!default_texture_)
    {
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, default_texture_);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool Gles2Immediate::Init()
{
    if (!CreateDefaultTexture())
    {
        return false;
    }

    glGenBuffers(1, &vertex_buffer_);

    if (!vertex_buffer_)
    {
        return false;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)kGles2VertexBufferBytes, nullptr, GL_STREAM_DRAW);

    if (!CreateQuadIndexBuffer())
    {
        return false;
    }

    for (int32_t i = 0; i < kGles2MatrixModeTotal; i++)
    {
        matrix_top_[i]       = 0;
        matrix_stack_[i][0]  = HMM_M4D(1.0f);
    }

    glEnableVertexAttribArray(kGles2AttributePosition);
    glEnableVertexAttribArray(kGles2AttributeTextureCoordinates);
    glEnableVertexAttribArray(kGles2AttributeColor);

    return true;
}

void Gles2Immediate::Shutdown()
{
    if (vertex_buffer_)
    {
        glDeleteBuffers(1, &vertex_buffer_);
        vertex_buffer_ = 0;
    }

    if (quad_index_buffer_)
    {
        glDeleteBuffers(1, &quad_index_buffer_);
        quad_index_buffer_ = 0;
    }

    if (default_texture_)
    {
        glDeleteTextures(1, &default_texture_);
        default_texture_ = 0;
    }
}

void Gles2Immediate::BeginFrame()
{
    vertex_buffer_offset_ = 0;

    InvalidateBatch();

    for (int32_t i = 0; i < kGles2MatrixModeTotal; i++)
    {
        matrix_top_[i]      = 0;
        matrix_stack_[i][0] = HMM_M4D(1.0f);
    }

    matrices_dirty_ = true;
}

void Gles2Immediate::LoadIdentity()
{
    matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]] = HMM_M4D(1.0f);

    MarkMatrixDirty();
}

void Gles2Immediate::PushMatrix()
{
    int32_t &top = matrix_top_[current_matrix_mode_];

    if (top + 1 >= kGles2MatrixStackDepth)
    {
        FatalError("Gles2Immediate: matrix stack overflow\n");
    }

    matrix_stack_[current_matrix_mode_][top + 1] = matrix_stack_[current_matrix_mode_][top];

    top++;

    MarkMatrixDirty();
}

void Gles2Immediate::PopMatrix()
{
    int32_t &top = matrix_top_[current_matrix_mode_];

    if (top == 0)
    {
        FatalError("Gles2Immediate: matrix stack underflow\n");
    }

    top--;

    MarkMatrixDirty();
}

void Gles2Immediate::LoadMatrix(const HMM_Mat4 &matrix)
{
    matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]] = matrix;

    MarkMatrixDirty();
}

void Gles2Immediate::MultiplyMatrix(const HMM_Mat4 &matrix)
{
    HMM_Mat4 &current = matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]];

    current = HMM_MulM4(current, matrix);

    MarkMatrixDirty();
}

void Gles2Immediate::Translate(float x, float y, float z)
{
    MultiplyMatrix(HMM_Translate(HMM_V3(x, y, z)));
}

void Gles2Immediate::Rotate(float radians, float x, float y, float z)
{
    MultiplyMatrix(HMM_Rotate_RH(radians, HMM_V3(x, y, z)));
}

void Gles2Immediate::Scale(float x, float y, float z)
{
    MultiplyMatrix(HMM_Scale(HMM_V3(x, y, z)));
}

void Gles2Immediate::Orthographic(float left, float right, float bottom, float top, float z_near, float z_far)
{
    MultiplyMatrix(Gles2OrthographicMatrix(left, right, bottom, top, z_near, z_far));
}

void Gles2Immediate::Frustum(float left, float right, float bottom, float top, float z_near, float z_far)
{
    MultiplyMatrix(Gles2FrustumMatrix(left, right, bottom, top, z_near, z_far));
}

void Gles2Immediate::ApplyMatrices()
{
    if (!matrices_dirty_)
    {
        return;
    }

    matrices_dirty_ = false;

    const HMM_Mat4 &model_view = matrix_stack_[kGles2MatrixModeModelView][matrix_top_[kGles2MatrixModeModelView]];
    const HMM_Mat4 &projection = matrix_stack_[kGles2MatrixModeProjection][matrix_top_[kGles2MatrixModeProjection]];

    gles2_program.SetModelView(model_view);
    gles2_program.SetModelViewProjection(HMM_MulM4(projection, model_view));
}

void Gles2Immediate::Viewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    glViewport(x, y, width, height);
}

void Gles2Immediate::ClearDepth()
{
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void Gles2Immediate::SetLineMode(bool enabled)
{
    gles2_program.SetLineMode(enabled);
}

void Gles2Immediate::InvalidateBatch()
{
    batch_base_   = nullptr;
    batch_count_  = 0;
    batch_offset_ = 0;
}

size_t Gles2Immediate::StreamVertices(const RendererVertex *vertices, int32_t count)
{
    size_t bytes = (size_t)count * sizeof(RendererVertex);

    if (bytes > kGles2VertexBufferBytes)
    {
        FatalError("Gles2Immediate: vertex batch of %zu bytes exceeds the stream buffer\n", bytes);
    }

    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);

    if (vertex_buffer_offset_ + bytes > kGles2VertexBufferBytes)
    {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)kGles2VertexBufferBytes, nullptr, GL_STREAM_DRAW);

        vertex_buffer_offset_ = 0;

        InvalidateBatch();
    }

    size_t offset = vertex_buffer_offset_;

    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)bytes, vertices);

    vertex_buffer_offset_ += bytes;

    uploaded_bytes_ += bytes;
    upload_count_++;

    return offset;
}

void Gles2Immediate::UploadBatch(const RendererVertex *vertices, int32_t count)
{
    if (!vertices || count <= 0)
    {
        InvalidateBatch();
        return;
    }

    batch_offset_ = StreamVertices(vertices, count);
    batch_base_   = vertices;
    batch_count_  = count;
}

void Gles2Immediate::BindVertexAttributes(size_t byte_offset)
{
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);

    glVertexAttribPointer(kGles2AttributePosition, 3, GL_FLOAT, GL_FALSE, sizeof(RendererVertex),
                          (const void *)(byte_offset + offsetof(RendererVertex, position)));

    glVertexAttribPointer(kGles2AttributeTextureCoordinates, 4, GL_FLOAT, GL_FALSE, sizeof(RendererVertex),
                          (const void *)(byte_offset + offsetof(RendererVertex, texture_coordinates)));

    glVertexAttribPointer(kGles2AttributeColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RendererVertex),
                          (const void *)(byte_offset + offsetof(RendererVertex, rgba)));
}

void Gles2Immediate::DrawRange(GLuint shape, size_t byte_offset, int32_t count)
{
    BindVertexAttributes(byte_offset);

    if (shape == GL_QUADS)
    {
        int32_t quads = count / 4;

        if (quads <= 0)
        {
            return;
        }

        if (quads > kGles2MaximumQuads)
        {
            FatalError("Gles2Immediate: %d quads exceeds the index buffer\n", quads);
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_index_buffer_);
        glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, (const void *)0);
    }
    else
    {
        GLenum mode = shape;

        if (shape == GL_POLYGON)
        {
            mode = GL_TRIANGLE_FAN;
        }
        else if (shape == GL_QUAD_STRIP)
        {
            mode = GL_TRIANGLE_STRIP;
        }

        glDrawArrays(mode, 0, count);
    }

    draw_count_++;
}

void Gles2Immediate::Draw(GLuint shape, const RendererVertex *vertices, int32_t base, int32_t count)
{
    if (!vertices || count <= 0)
    {
        return;
    }

    ApplyMatrices();

    if (batch_base_ == vertices && base >= 0 && base + count <= batch_count_)
    {
        DrawRange(shape, batch_offset_ + (size_t)base * sizeof(RendererVertex), count);
        return;
    }

    size_t offset = StreamVertices(vertices + base, count);

    DrawRange(shape, offset, count);
}

RendererVertex *Gles2Immediate::ReserveVertices(int32_t count)
{
    if (count <= 0)
    {
        return nullptr;
    }

    scratch_vertices_.resize((size_t)count);

    return scratch_vertices_.data();
}

void Gles2Immediate::RecordDraw(GLuint shape, int32_t count)
{
    if (count <= 0 || (size_t)count > scratch_vertices_.size())
    {
        return;
    }

    ApplyMatrices();

    size_t offset = StreamVertices(scratch_vertices_.data(), count);

    DrawRange(shape, offset, count);
}

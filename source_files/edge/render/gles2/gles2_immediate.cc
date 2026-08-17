#include "gles2_immediate.h"

#include <stddef.h>
#include <string.h>

#include "epi.h"
#include "i_system.h"
#include "r_backend.h"

Gles2Immediate gles2_immediate;

static_assert(sizeof(RendererVertex) == 32, "RendererVertex size");
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
    DestroyRenderTarget();

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

    if (merged_index_buffer_)
    {
        glDeleteBuffers(1, &merged_index_buffer_);
        merged_index_buffer_ = 0;
    }

    if (model_index_buffer_)
    {
        glDeleteBuffers(1, &model_index_buffer_);
        model_index_buffer_ = 0;
    }

    if (model_index_buffer_)
    {
        glDeleteBuffers(1, &model_index_buffer_);
        model_index_buffer_ = 0;
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
    glViewport(render_backend->ScaleToRenderTargetX(x), render_backend->ScaleToRenderTargetY(y),
               render_backend->ScaleToRenderTargetX(width), render_backend->ScaleToRenderTargetY(height));
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

void Gles2Immediate::BindVertexAttributesFrom(GLuint buffer)
{
    glBindBuffer(GL_ARRAY_BUFFER, buffer);

    glVertexAttribPointer(kGles2AttributePosition, 3, GL_FLOAT, GL_FALSE, sizeof(RendererVertex),
                          (const void *)offsetof(RendererVertex, position));

    glVertexAttribPointer(kGles2AttributeTextureCoordinates, 4, GL_FLOAT, GL_FALSE, sizeof(RendererVertex),
                          (const void *)offsetof(RendererVertex, texture_coordinates));

    glVertexAttribPointer(kGles2AttributeColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RendererVertex),
                          (const void *)offsetof(RendererVertex, rgba));
}

GLuint Gles2Immediate::CreateStaticBuffer(const RendererVertex *vertices, int count)
{
    if (!vertices || count <= 0)
        return 0;

    GLuint buffer = 0;

    glGenBuffers(1, &buffer);

    if (!buffer)
        return 0;

    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * sizeof(RendererVertex)), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);

    return buffer;
}

void Gles2Immediate::DeleteStaticBuffer(GLuint buffer)
{
    if (!buffer)
        return;

    glDeleteBuffers(1, &buffer);

    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
}

void Gles2Immediate::DrawStatic(GLuint buffer, GLuint shape, int first, int count)
{
    if (!buffer || count <= 0)
        return;

    ApplyMatrices();

    BindVertexAttributesFrom(buffer);

    glDrawArrays(shape, first, count);

    draw_count_++;

    BindVertexAttributes(batch_offset_);
}

void Gles2Immediate::UploadMergedIndices(const uint16_t *indices, int32_t count)
{
    if (!indices || count <= 0)
        return;

    if (!merged_index_buffer_)
    {
        glGenBuffers(1, &merged_index_buffer_);

        if (!merged_index_buffer_)
            return;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, merged_index_buffer_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * sizeof(uint16_t)), indices, GL_STREAM_DRAW);

    uploaded_bytes_ += (size_t)count * sizeof(uint16_t);
    upload_count_++;
}

void Gles2Immediate::DrawMerged(int32_t index_offset, int32_t index_count)
{
    if (index_count <= 0 || !merged_index_buffer_)
        return;

    ApplyMatrices();

    BindVertexAttributes(batch_offset_);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, merged_index_buffer_);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT,
                   (const void *)((size_t)index_offset * sizeof(uint16_t)));

    draw_count_++;
}

void Gles2Immediate::DrawMergedWithoutMatrices(int32_t index_offset, int32_t index_count)
{
    if (index_count <= 0 || !merged_index_buffer_)
        return;

    BindVertexAttributes(batch_offset_);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, merged_index_buffer_);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT,
                   (const void *)((size_t)index_offset * sizeof(uint16_t)));

    draw_count_++;
}

void Gles2Immediate::UploadModelIndices(const uint16_t *indices, int32_t count)
{
    if (!indices || count <= 0)
        return;

    if (!model_index_buffer_)
    {
        glGenBuffers(1, &model_index_buffer_);

        if (!model_index_buffer_)
            return;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_index_buffer_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * sizeof(uint16_t)), indices, GL_STREAM_DRAW);

    uploaded_bytes_ += (size_t)count * sizeof(uint16_t);
    upload_count_++;
}

void Gles2Immediate::DrawModelIndexed(int32_t index_offset, int32_t index_count)
{
    if (index_count <= 0 || !model_index_buffer_)
        return;

    ApplyMatrices();

    BindVertexAttributes(batch_offset_);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, model_index_buffer_);
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT,
                   (const void *)((size_t)index_offset * sizeof(uint16_t)));

    draw_count_++;
}

uint32_t Gles2Immediate::CreateModelMesh(const ModelMeshData &data, const uint16_t *indices, int32_t index_count)
{
    if (!data.frame_positions || !data.texture_coordinates || !indices)
        return 0;

    if (data.vertex_count <= 0 || data.frame_count <= 0 || index_count <= 0)
        return 0;

    Gles2ModelMesh mesh;

    mesh.vertex_count = data.vertex_count;
    mesh.frame_count  = data.frame_count;

    glGenBuffers(1, &mesh.position_buffer);
    glGenBuffers(1, &mesh.texture_coordinate_buffer);
    glGenBuffers(1, &mesh.color_buffer);
    glGenBuffers(1, &mesh.index_buffer);

    if (!mesh.position_buffer || !mesh.texture_coordinate_buffer || !mesh.color_buffer || !mesh.index_buffer)
        return 0;

    size_t position_bytes = (size_t)data.vertex_count * (size_t)data.frame_count * 3 * sizeof(float);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.position_buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)position_bytes, data.frame_positions, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.texture_coordinate_buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)data.vertex_count * 2 * sizeof(float)),
                 data.texture_coordinates, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.color_buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)data.vertex_count * 6 * sizeof(float)), nullptr,
                 GL_STREAM_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)index_count * sizeof(uint16_t)), indices,
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    model_meshes_.push_back(mesh);

    return (uint32_t)model_meshes_.size();
}

void Gles2Immediate::DeleteModelMesh(uint32_t handle)
{
    if (handle == 0 || handle > model_meshes_.size())
        return;

    Gles2ModelMesh *mesh = &model_meshes_[handle - 1];

    if (mesh->position_buffer)
        glDeleteBuffers(1, &mesh->position_buffer);

    if (mesh->texture_coordinate_buffer)
        glDeleteBuffers(1, &mesh->texture_coordinate_buffer);

    if (mesh->color_buffer)
        glDeleteBuffers(1, &mesh->color_buffer);

    if (mesh->index_buffer)
        glDeleteBuffers(1, &mesh->index_buffer);

    EPI_CLEAR_MEMORY(mesh, Gles2ModelMesh, 1);
}

void Gles2Immediate::UpdateModelColors(uint32_t handle, const float *colors, int32_t vertex_count)
{
    if (handle == 0 || handle > model_meshes_.size() || !colors || vertex_count <= 0)
        return;

    Gles2ModelMesh *mesh = &model_meshes_[handle - 1];

    if (!mesh->color_buffer || vertex_count > mesh->vertex_count)
        return;

    size_t bytes = (size_t)vertex_count * 6 * sizeof(float);

    glBindBuffer(GL_ARRAY_BUFFER, mesh->color_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, colors);

    uploaded_bytes_ += bytes;
    upload_count_++;
}

void Gles2Immediate::BindModelMesh(const ModelDrawInfo &info)
{
    Gles2ModelMesh *mesh = &model_meshes_[info.handle - 1];

    size_t frame_stride = (size_t)mesh->vertex_count * 3 * sizeof(float);
    size_t vertex_base  = (size_t)info.first_vertex * 3 * sizeof(float);

    glBindBuffer(GL_ARRAY_BUFFER, mesh->position_buffer);

    glVertexAttribPointer(kGles2AttributeModelPositionFrame1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (const void *)((size_t)info.frame1 * frame_stride + vertex_base));

    glVertexAttribPointer(kGles2AttributeModelPositionFrame2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (const void *)((size_t)info.frame2 * frame_stride + vertex_base));

    glBindBuffer(GL_ARRAY_BUFFER, mesh->texture_coordinate_buffer);

    glVertexAttribPointer(kGles2AttributeModelTextureCoordinates, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                          (const void *)((size_t)info.first_vertex * 2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, mesh->color_buffer);

    size_t color_offset = (size_t)info.first_vertex * 6 * sizeof(float) + (info.additive_pass ? 3 * sizeof(float) : 0);

    glVertexAttribPointer(kGles2AttributeModelColor, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (const void *)color_offset);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->index_buffer);
}

void Gles2Immediate::DrawModelMesh(const ModelDrawInfo &info)
{
    if (info.handle == 0 || info.handle > model_meshes_.size() || info.index_count <= 0)
        return;

    Gles2ModelMesh *mesh = &model_meshes_[info.handle - 1];

    if (!mesh->position_buffer || info.frame1 >= mesh->frame_count || info.frame2 >= mesh->frame_count)
        return;

    glEnableVertexAttribArray(kGles2AttributeModelColor);

    BindModelMesh(info);

    glDrawElements(GL_TRIANGLES, info.index_count, GL_UNSIGNED_SHORT,
                   (const void *)((size_t)info.first_index * sizeof(uint16_t)));

    glDisableVertexAttribArray(kGles2AttributeModelColor);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    draw_count_++;
}

void Gles2Immediate::DrawMovieQuad(const RendererVertex *vertices)
{
    if (!vertices)
        return;

    size_t offset = StreamVertices(vertices, 4);

    BindVertexAttributes(offset);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_index_buffer_);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);

    InvalidateBatch();

    draw_count_++;
}

static int32_t Gles2NextPowerOfTwo(int32_t value)
{
    int32_t result = 1;

    while (result < value)
        result <<= 1;

    return result;
}

bool Gles2Immediate::AttachRenderTargetDepth(int32_t width, int32_t height)
{
    struct Gles2DepthFormat
    {
        GLenum internal_format;
        GLenum attachment;
        bool   separate_stencil;
        bool   has_stencil;
    };

    static const Gles2DepthFormat formats[4] = {{GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT, false, true},
                                                {GL_DEPTH_STENCIL, GL_DEPTH_STENCIL_ATTACHMENT, false, true},
                                                {GL_DEPTH24_STENCIL8, GL_DEPTH_ATTACHMENT, true, true},
                                                {GL_DEPTH_COMPONENT16, GL_DEPTH_ATTACHMENT, false, false}};

    for (int32_t i = 0; i < 4; i++)
    {
        if (render_target_depth_)
        {
            glDeleteRenderbuffers(1, &render_target_depth_);
            render_target_depth_ = 0;
        }

        glGenRenderbuffers(1, &render_target_depth_);

        if (!render_target_depth_)
            return false;

        glBindRenderbuffer(GL_RENDERBUFFER, render_target_depth_);
        glRenderbufferStorage(GL_RENDERBUFFER, formats[i].internal_format, width, height);

        glFramebufferRenderbuffer(GL_FRAMEBUFFER, formats[i].attachment, GL_RENDERBUFFER, render_target_depth_);

        if (formats[i].separate_stencil)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, render_target_depth_);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        {
            render_target_has_stencil_ = formats[i].has_stencil;
            return true;
        }
    }

    if (render_target_depth_)
    {
        glDeleteRenderbuffers(1, &render_target_depth_);
        render_target_depth_ = 0;
    }

    return false;
}

bool Gles2Immediate::EnsureRenderTarget(int32_t width, int32_t height)
{
    if (width < 1 || height < 1 || !Gles2HasFramebufferObjects())
        return false;

    if (render_target_framebuffer_ && render_target_width_ == width && render_target_height_ == height)
        return true;

    if (CreateRenderTarget(width, height, width, height))
        return true;

    return CreateRenderTarget(width, height, Gles2NextPowerOfTwo(width), Gles2NextPowerOfTwo(height));
}

bool Gles2Immediate::CreateRenderTarget(int32_t width, int32_t height, int32_t texture_width, int32_t texture_height)
{
    DestroyRenderTarget();

    glGenFramebuffers(1, &render_target_framebuffer_);

    if (!render_target_framebuffer_)
        return false;

    glBindFramebuffer(GL_FRAMEBUFFER, render_target_framebuffer_);

    glGenTextures(1, &render_target_color_);

    if (!render_target_color_)
    {
        DestroyRenderTarget();
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, render_target_color_);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width, texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_target_color_, 0);

    if (!AttachRenderTargetDepth(texture_width, texture_height))
    {
        DestroyRenderTarget();
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    render_target_width_          = width;
    render_target_height_         = height;
    render_target_texture_width_  = texture_width;
    render_target_texture_height_ = texture_height;

    return true;
}

void Gles2Immediate::DestroyRenderTarget()
{
    if (render_target_framebuffer_)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &render_target_framebuffer_);
        render_target_framebuffer_ = 0;
    }

    if (render_target_color_)
    {
        glDeleteTextures(1, &render_target_color_);
        render_target_color_ = 0;
    }

    if (render_target_depth_)
    {
        glDeleteRenderbuffers(1, &render_target_depth_);
        render_target_depth_ = 0;
    }

    render_target_width_          = 0;
    render_target_height_         = 0;
    render_target_texture_width_  = 0;
    render_target_texture_height_ = 0;
    render_target_has_stencil_    = false;
}

void Gles2Immediate::BindRenderTarget()
{
    if (!Gles2HasFramebufferObjects())
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, render_target_framebuffer_);
}

void Gles2Immediate::ResolveRenderTarget(const Gles2ResolveRect &source, const Gles2ResolveRect &destination,
                                         int32_t window_width, int32_t window_height, bool smooth)
{
    if (!render_target_framebuffer_)
        return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glViewport(0, 0, window_width, window_height);

    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);

    glDepthMask(GL_FALSE);

    gles2_program.Use();

    gles2_program.SetModelViewProjection(HMM_M4D(1.0f));
    gles2_program.SetModelView(HMM_M4D(1.0f));
    gles2_program.SetMultiTexture(false);
    gles2_program.SetLineMode(false);
    gles2_program.SetSkipRGB(false);
    gles2_program.SetAlphaTest(0.0f);
    gles2_program.SetFog(kGles2FogModeNone, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    gles2_program.SetSkyPass(nullptr);

    for (int32_t i = 0; i < kGles2MaximumClipPlanes; i++)
        gles2_program.SetClipPlaneEnabled(i, false);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, default_texture_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, render_target_color_);

    GLint filter = smooth ? GL_LINEAR : GL_NEAREST;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    float u0 = (float)source.x / (float)render_target_texture_width_;
    float v0 = (float)source.y / (float)render_target_texture_height_;
    float u1 = (float)(source.x + source.width) / (float)render_target_texture_width_;
    float v1 = (float)(source.y + source.height) / (float)render_target_texture_height_;

    float x0 = (float)destination.x / (float)window_width * 2.0f - 1.0f;
    float y0 = (float)destination.y / (float)window_height * 2.0f - 1.0f;
    float x1 = (float)(destination.x + destination.width) / (float)window_width * 2.0f - 1.0f;
    float y1 = (float)(destination.y + destination.height) / (float)window_height * 2.0f - 1.0f;

    RendererVertex quad[4];

    EPI_CLEAR_MEMORY(quad, RendererVertex, 4);

    for (int32_t i = 0; i < 4; i++)
        quad[i].rgba = kRGBAWhite;

    quad[0].position               = {{x0, y0, 0.0f}};
    quad[0].texture_coordinates[0] = {{u0, v0}};

    quad[1].position               = {{x1, y0, 0.0f}};
    quad[1].texture_coordinates[0] = {{u1, v0}};

    quad[2].position               = {{x1, y1, 0.0f}};
    quad[2].texture_coordinates[0] = {{u1, v1}};

    quad[3].position               = {{x0, y1, 0.0f}};
    quad[3].texture_coordinates[0] = {{u0, v1}};

    size_t offset = StreamVertices(quad, 4);

    BindVertexAttributes(offset);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_index_buffer_);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);

    InvalidateBatch();

    draw_count_++;

    MarkMatrixDirty();
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

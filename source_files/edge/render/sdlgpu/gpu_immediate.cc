#include "gpu_immediate.h"

#include <math.h>
#include <string.h>

#include "epi.h"
#include "epi_math.h"
#include "gpu_device.h"
#include "gpu_matrix.h"
#include "i_system.h"

GpuImmediate gpu_immediate;

static constexpr int32_t kGpuQuadIndexTotal = kGpuMaximumQuads * 6;
static constexpr int32_t kGpuFanIndexTotal  = (kGpuMaximumFanVertices - 2) * 3;

static constexpr size_t kGpuInitialVertexCapacity = 64 * 1024 * sizeof(RendererVertex);

bool GpuImmediate::Init(SDL_GPUDevice *device)
{
    device_ = device;

    if (!CreateIndexBuffers(device))
        return false;

    SDL_GPUTextureCreateInfo texture_info;
    EPI_CLEAR_MEMORY(&texture_info, SDL_GPUTextureCreateInfo, 1);

    texture_info.type                 = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width                = 1;
    texture_info.height               = 1;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels           = 1;
    texture_info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    default_texture_ = SDL_CreateGPUTexture(device, &texture_info);

    if (!default_texture_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUTexture (default) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler_info;
    EPI_CLEAR_MEMORY(&sampler_info, SDL_GPUSamplerCreateInfo, 1);

    sampler_info.min_filter     = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter     = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    default_sampler_ = SDL_CreateGPUSampler(device, &sampler_info);

    if (!default_sampler_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUSampler (default) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = 4;

    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

    if (!transfer)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUTransferBuffer (default texture) failed: %s\n", SDL_GetError());
        return false;
    }

    uint8_t *pixels = (uint8_t *)SDL_MapGPUTransferBuffer(device, transfer, false);

    if (!pixels)
    {
        LogPrint("GpuImmediate: SDL_MapGPUTransferBuffer (default texture) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }

    pixels[0] = 255;
    pixels[1] = 255;
    pixels[2] = 255;
    pixels[3] = 255;

    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);

    if (!command_buffer)
    {
        LogPrint("GpuImmediate: SDL_AcquireGPUCommandBuffer (default texture) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    SDL_GPUTextureTransferInfo source;
    EPI_CLEAR_MEMORY(&source, SDL_GPUTextureTransferInfo, 1);
    source.transfer_buffer = transfer;

    SDL_GPUTextureRegion destination;
    EPI_CLEAR_MEMORY(&destination, SDL_GPUTextureRegion, 1);
    destination.texture = default_texture_;
    destination.w       = 1;
    destination.h       = 1;
    destination.d       = 1;

    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);

    SDL_ReleaseGPUTransferBuffer(device, transfer);

    current_texture_[0] = default_texture_;
    current_texture_[1] = default_texture_;
    current_sampler_[0] = default_sampler_;
    current_sampler_[1] = default_sampler_;

    EPI_CLEAR_MEMORY(&current_fragment_parameters_, GpuFragmentParameters, 1);
    memset(clip_plane_, 0, sizeof(clip_plane_));

    for (int32_t i = 0; i < kGpuMatrixModeTotal; i++)
    {
        matrix_top_[i]      = 0;
        matrix_stack_[i][0] = HMM_M4D(1.0f);
    }

    return true;
}

bool GpuImmediate::CreateIndexBuffers(SDL_GPUDevice *device)
{
    size_t quad_bytes = (size_t)kGpuQuadIndexTotal * sizeof(uint16_t);
    size_t fan_bytes  = (size_t)kGpuFanIndexTotal * sizeof(uint16_t);

    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    buffer_info.size  = (uint32_t)quad_bytes;

    quad_index_buffer_ = SDL_CreateGPUBuffer(device, &buffer_info);

    buffer_info.size  = (uint32_t)fan_bytes;
    fan_index_buffer_ = SDL_CreateGPUBuffer(device, &buffer_info);

    if (!quad_index_buffer_ || !fan_index_buffer_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUBuffer (index) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)(quad_bytes + fan_bytes);

    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

    if (!transfer)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUTransferBuffer (index) failed: %s\n", SDL_GetError());
        return false;
    }

    uint16_t *indices = (uint16_t *)SDL_MapGPUTransferBuffer(device, transfer, false);

    if (!indices)
    {
        LogPrint("GpuImmediate: SDL_MapGPUTransferBuffer (index) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }

    for (int32_t quad = 0; quad < kGpuMaximumQuads; quad++)
    {
        uint16_t base = (uint16_t)(quad * 4);
        uint16_t *out = indices + quad * 6;

        out[0] = base;
        out[1] = (uint16_t)(base + 1);
        out[2] = (uint16_t)(base + 2);
        out[3] = base;
        out[4] = (uint16_t)(base + 2);
        out[5] = (uint16_t)(base + 3);
    }

    uint16_t *fan_indices = indices + kGpuQuadIndexTotal;

    for (int32_t triangle = 0; triangle < kGpuMaximumFanVertices - 2; triangle++)
    {
        uint16_t *out = fan_indices + triangle * 3;

        out[0] = 0;
        out[1] = (uint16_t)(triangle + 1);
        out[2] = (uint16_t)(triangle + 2);
    }

    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);

    if (!command_buffer)
    {
        LogPrint("GpuImmediate: SDL_AcquireGPUCommandBuffer (index) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    SDL_GPUTransferBufferLocation source;
    SDL_GPUBufferRegion           destination;

    source.transfer_buffer = transfer;
    source.offset          = 0;

    destination.buffer = quad_index_buffer_;
    destination.offset = 0;
    destination.size   = (uint32_t)quad_bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);

    source.offset = (uint32_t)quad_bytes;

    destination.buffer = fan_index_buffer_;
    destination.offset = 0;
    destination.size   = (uint32_t)fan_bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);

    SDL_ReleaseGPUTransferBuffer(device, transfer);

    return true;
}

void GpuImmediate::Shutdown(SDL_GPUDevice *device)
{
    if (!device)
        return;

    if (vertex_buffer_)
    {
        SDL_ReleaseGPUBuffer(device, vertex_buffer_);
        vertex_buffer_ = nullptr;
    }

    if (vertex_transfer_buffer_)
    {
        SDL_ReleaseGPUTransferBuffer(device, vertex_transfer_buffer_);
        vertex_transfer_buffer_ = nullptr;
    }

    if (quad_index_buffer_)
    {
        SDL_ReleaseGPUBuffer(device, quad_index_buffer_);
        quad_index_buffer_ = nullptr;
    }

    if (fan_index_buffer_)
    {
        SDL_ReleaseGPUBuffer(device, fan_index_buffer_);
        fan_index_buffer_ = nullptr;
    }

    if (dynamic_index_buffer_)
    {
        SDL_ReleaseGPUBuffer(device, dynamic_index_buffer_);
        dynamic_index_buffer_ = nullptr;
    }

    if (dynamic_index_transfer_buffer_)
    {
        SDL_ReleaseGPUTransferBuffer(device, dynamic_index_transfer_buffer_);
        dynamic_index_transfer_buffer_ = nullptr;
    }

    dynamic_index_capacity_ = 0;

    if (default_texture_)
    {
        SDL_ReleaseGPUTexture(device, default_texture_);
        default_texture_ = nullptr;
    }

    if (default_sampler_)
    {
        SDL_ReleaseGPUSampler(device, default_sampler_);
        default_sampler_ = nullptr;
    }

    vertex_buffer_capacity_ = 0;
    device_                 = nullptr;
}

void GpuImmediate::BeginFrame()
{
    vertex_count_ = 0;
    dynamic_indices_.clear();
    commands_.clear();
    vertex_parameters_.clear();
    fragment_parameters_.clear();

    for (int32_t i = 0; i < kGpuMatrixModeTotal; i++)
    {
        matrix_top_[i]      = 0;
        matrix_stack_[i][0] = HMM_M4D(1.0f);
    }

    current_matrix_mode_ = kGpuMatrixModeModelView;

    memset(clip_plane_, 0, sizeof(clip_plane_));
    EPI_CLEAR_MEMORY(&current_fragment_parameters_, GpuFragmentParameters, 1);

    vertex_parameters_dirty_   = true;
    fragment_parameters_dirty_ = true;

    vertex_parameter_index_   = -1;
    fragment_parameter_index_ = -1;

    pipeline_flags_    = 0;
    source_blend_      = GL_SRC_ALPHA;
    destination_blend_ = GL_ONE_MINUS_SRC_ALPHA;

    current_texture_[0] = default_texture_;
    current_texture_[1] = default_texture_;
    current_sampler_[0] = default_sampler_;
    current_sampler_[1] = default_sampler_;

    texturing_enabled_ = false;

    pending_base_  = 0;
    pending_count_ = 0;

    viewport_set_ = false;
    scissor_set_  = false;
}

void GpuImmediate::LoadIdentity()
{
    matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]] = HMM_M4D(1.0f);
    MarkMatrixDirty();
}

void GpuImmediate::PushMatrix()
{
    int32_t top = matrix_top_[current_matrix_mode_];

    if (top + 1 >= kGpuMatrixStackDepth)
    {
        FatalError("GpuImmediate: matrix stack overflow\n");
    }

    matrix_stack_[current_matrix_mode_][top + 1] = matrix_stack_[current_matrix_mode_][top];
    matrix_top_[current_matrix_mode_]            = top + 1;

    MarkMatrixDirty();
}

void GpuImmediate::PopMatrix()
{
    if (matrix_top_[current_matrix_mode_] == 0)
    {
        FatalError("GpuImmediate: matrix stack underflow\n");
    }

    matrix_top_[current_matrix_mode_]--;

    MarkMatrixDirty();
}

void GpuImmediate::LoadMatrix(const HMM_Mat4 &matrix)
{
    matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]] = matrix;
    MarkMatrixDirty();
}

void GpuImmediate::MultiplyMatrix(const HMM_Mat4 &matrix)
{
    HMM_Mat4 &current = matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]];

    current = HMM_MulM4(current, matrix);

    MarkMatrixDirty();
}

void GpuImmediate::Translate(float x, float y, float z)
{
    MultiplyMatrix(HMM_Translate(HMM_V3(x, y, z)));
}

void GpuImmediate::Rotate(float radians, float x, float y, float z)
{
    if (sqrtf(x * x + y * y + z * z) < 1.0e-4f)
        return;

    MultiplyMatrix(HMM_Rotate_RH(radians, HMM_V3(x, y, z)));
}

void GpuImmediate::Scale(float x, float y, float z)
{
    MultiplyMatrix(HMM_Scale(HMM_V3(x, y, z)));
}

void GpuImmediate::Orthographic(float left, float right, float bottom, float top, float z_near, float z_far)
{
    MultiplyMatrix(GpuOrthographicMatrix(left, right, bottom, top, z_near, z_far));
}

void GpuImmediate::Frustum(float left, float right, float bottom, float top, float z_near, float z_far)
{
    MultiplyMatrix(GpuFrustumMatrix(left, right, bottom, top, z_near, z_far));
}

void GpuImmediate::SetPipelineState(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend)
{
    pipeline_flags_    = pipeline_flags;
    source_blend_      = source_blend;
    destination_blend_ = destination_blend;
}

void GpuImmediate::SetTexture(SDL_GPUTexture *texture, SDL_GPUSampler *sampler)
{
    current_texture_[0] = texture ? texture : default_texture_;
    current_sampler_[0] = sampler ? sampler : default_sampler_;
    current_texture_[1] = default_texture_;
    current_sampler_[1] = default_sampler_;

    texturing_enabled_ = true;

    if (current_fragment_parameters_.flags & kGpuFragmentFlagMultiTexture)
    {
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagMultiTexture;
        fragment_parameters_dirty_ = true;
    }
}

void GpuImmediate::SetMultiTexture(SDL_GPUTexture *texture0, SDL_GPUSampler *sampler0, SDL_GPUTexture *texture1,
                                   SDL_GPUSampler *sampler1)
{
    current_texture_[0] = texture0 ? texture0 : default_texture_;
    current_sampler_[0] = sampler0 ? sampler0 : default_sampler_;
    current_texture_[1] = texture1 ? texture1 : default_texture_;
    current_sampler_[1] = sampler1 ? sampler1 : default_sampler_;

    texturing_enabled_ = true;

    if (!(current_fragment_parameters_.flags & kGpuFragmentFlagMultiTexture))
    {
        current_fragment_parameters_.flags |= kGpuFragmentFlagMultiTexture;
        fragment_parameters_dirty_ = true;
    }
}

void GpuImmediate::DisableTexture()
{
    texturing_enabled_ = false;

    if (current_fragment_parameters_.flags & kGpuFragmentFlagMultiTexture)
    {
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagMultiTexture;
        fragment_parameters_dirty_ = true;
    }
}

void GpuImmediate::SetAlphaTest(float alpha_test)
{
    if (!epi::AlmostEquals(current_fragment_parameters_.alpha_test, alpha_test))
    {
        current_fragment_parameters_.alpha_test = alpha_test;
        fragment_parameters_dirty_              = true;
    }
}

void GpuImmediate::SetFog(GpuFogMode mode, float red, float green, float blue, float alpha, float density, float start,
                          float end, float scale)
{
    GpuFragmentParameters *parameters = &current_fragment_parameters_;

    if (parameters->fog_mode == (int32_t)mode && epi::AlmostEquals(parameters->fog_color[0], red) &&
        epi::AlmostEquals(parameters->fog_color[1], green) && epi::AlmostEquals(parameters->fog_color[2], blue) &&
        epi::AlmostEquals(parameters->fog_color[3], alpha) && epi::AlmostEquals(parameters->fog_density, density) &&
        epi::AlmostEquals(parameters->fog_start, start) && epi::AlmostEquals(parameters->fog_end, end) &&
        epi::AlmostEquals(parameters->fog_scale, scale))
    {
        return;
    }

    parameters->fog_mode     = (int32_t)mode;
    parameters->fog_color[0] = red;
    parameters->fog_color[1] = green;
    parameters->fog_color[2] = blue;
    parameters->fog_color[3] = alpha;
    parameters->fog_density  = density;
    parameters->fog_start    = start;
    parameters->fog_end      = end;
    parameters->fog_scale    = scale;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetLineMode(bool enabled)
{
    bool current = (current_fragment_parameters_.flags & kGpuFragmentFlagLine) != 0;

    if (current == enabled)
        return;

    if (enabled)
        current_fragment_parameters_.flags |= kGpuFragmentFlagLine;
    else
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagLine;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetClipPlane(int32_t index, const double equation[4])
{
    EPI_ASSERT(index >= 0 && index < kGpuMaximumClipPlanes);

    HMM_Mat4 inverse = HMM_InvGeneralM4(ModelViewMatrix());

    HMM_Vec4 plane = HMM_V4((float)equation[0], (float)equation[1], (float)equation[2], (float)equation[3]);

    for (int32_t i = 0; i < 4; i++)
        clip_plane_[index][i] = HMM_DotV4(inverse.Columns[i], plane);

    vertex_parameters_dirty_ = true;
}

void GpuImmediate::SetClipPlaneEnabled(int32_t index, bool enabled)
{
    EPI_ASSERT(index >= 0 && index < kGpuMaximumClipPlanes);

    int32_t mask    = 1 << index;
    int32_t current = current_fragment_parameters_.clipplanes;

    if (enabled == ((current & mask) != 0))
        return;

    if (enabled)
        current_fragment_parameters_.clipplanes = current | mask;
    else
        current_fragment_parameters_.clipplanes = current & ~mask;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::Viewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    GpuCommand command;

    command.type                   = kGpuCommandViewport;
    command.arguments.rectangle.x  = x;
    command.arguments.rectangle.y  = y;
    command.arguments.rectangle.width  = width;
    command.arguments.rectangle.height = height;

    commands_.push_back(command);
}

void GpuImmediate::ScissorRect(int32_t x, int32_t y, int32_t width, int32_t height)
{
    GpuCommand command;

    command.type                       = kGpuCommandScissor;
    command.arguments.rectangle.x      = x;
    command.arguments.rectangle.y      = y;
    command.arguments.rectangle.width  = width;
    command.arguments.rectangle.height = height;

    commands_.push_back(command);
}

void GpuImmediate::ClearDepth()
{
    GpuCommand command;

    command.type = kGpuCommandClearDepth;

    commands_.push_back(command);
}

int32_t GpuImmediate::CurrentVertexParameters()
{
    if (!vertex_parameters_dirty_ && vertex_parameter_index_ >= 0)
        return vertex_parameter_index_;

    vertex_parameters_dirty_ = false;

    GpuVertexParameters parameters;

    parameters.mv  = matrix_stack_[kGpuMatrixModeModelView][matrix_top_[kGpuMatrixModeModelView]];
    parameters.mvp = HMM_MulM4(matrix_stack_[kGpuMatrixModeProjection][matrix_top_[kGpuMatrixModeProjection]],
                               parameters.mv);
    parameters.tm  = matrix_stack_[kGpuMatrixModeTexture][matrix_top_[kGpuMatrixModeTexture]];

    memcpy(parameters.clipplane, clip_plane_, sizeof(clip_plane_));

    vertex_parameters_.push_back(parameters);

    vertex_parameter_index_ = (int32_t)vertex_parameters_.size() - 1;

    return vertex_parameter_index_;
}

int32_t GpuImmediate::CurrentFragmentParameters()
{
    if (!fragment_parameters_dirty_ && fragment_parameter_index_ >= 0)
        return fragment_parameter_index_;

    fragment_parameters_dirty_ = false;

    fragment_parameters_.push_back(current_fragment_parameters_);

    fragment_parameter_index_ = (int32_t)fragment_parameters_.size() - 1;

    return fragment_parameter_index_;
}

RendererVertex *GpuImmediate::ReserveVertices(int32_t count)
{
    EPI_ASSERT(count > 0);

    size_t required = (size_t)vertex_count_ + (size_t)count;

    if (required > vertices_.size())
    {
        size_t capacity = vertices_.empty() ? (size_t)(64 * 1024) : vertices_.size();

        while (capacity < required)
            capacity *= 2;

        vertices_.resize(capacity);
    }

    pending_base_  = vertex_count_;
    pending_count_ = count;

    vertex_count_ += count;

    return vertices_.data() + pending_base_;
}

void GpuImmediate::RecordDraw(GLuint shape, int32_t count)
{
    EPI_ASSERT(count <= pending_count_);

    if (count <= 0)
        return;

    GpuPrimitiveType primitive    = kGpuPrimitiveTriangleList;
    GpuIndexSource   index_source = kGpuIndexSourceNone;
    int32_t          index_count  = 0;
    bool             mergeable    = true;

    switch (shape)
    {
    case GL_QUADS:
        count -= count % 4;
        if (count < 4)
            return;
        index_source = kGpuIndexSourceDynamic;
        break;

    case GL_TRIANGLES:
        count -= count % 3;
        if (count < 3)
            return;
        index_source = kGpuIndexSourceDynamic;
        break;

    case GL_POLYGON:
    case GL_TRIANGLE_FAN:
    case GL_QUAD_STRIP:
    case GL_TRIANGLE_STRIP:
        if (count < 3)
            return;
        index_source = kGpuIndexSourceDynamic;
        break;

    case GL_LINES:
        count -= count % 2;
        if (count < 2)
            return;
        primitive = kGpuPrimitiveLineList;
        mergeable = false;
        break;

    default:
        FatalError("GpuImmediate: unsupported shape 0x%04X\n", shape);
    }

    int32_t vertex_parameters   = CurrentVertexParameters();
    int32_t fragment_parameters = CurrentFragmentParameters();

    SDL_GPUTexture *texture0 = texturing_enabled_ ? current_texture_[0] : default_texture_;
    SDL_GPUSampler *sampler0 = texturing_enabled_ ? current_sampler_[0] : default_sampler_;
    SDL_GPUTexture *texture1 = texturing_enabled_ ? current_texture_[1] : default_texture_;
    SDL_GPUSampler *sampler1 = texturing_enabled_ ? current_sampler_[1] : default_sampler_;

    SDL_GPUGraphicsPipeline *pipeline = GetPipeline(pipeline_flags_, source_blend_, destination_blend_, primitive);

    if (mergeable && !commands_.empty())
    {
        GpuCommand *previous = &commands_.back();

        if (previous->type == kGpuCommandDraw)
        {
            GpuDrawArguments *draw = &previous->arguments.draw;

            if (draw->mergeable && draw->pipeline == pipeline && draw->index_source == index_source &&
                index_source == kGpuIndexSourceDynamic && draw->texture[0] == texture0 &&
                draw->sampler[0] == sampler0 && draw->texture[1] == texture1 && draw->sampler[1] == sampler1 &&
                draw->vertex_parameter_index == vertex_parameters &&
                draw->fragment_parameter_index == fragment_parameters &&
                draw->base_vertex + draw->vertex_count == pending_base_ &&
                draw->index_first + draw->index_count == (int32_t)dynamic_indices_.size() &&
                draw->vertex_count + count <= 65536)
            {
                draw->index_count += AppendDynamicIndices(shape, count, draw->vertex_count);
                draw->vertex_count += count;
                return;
            }
        }
    }

    GpuCommand command;

    command.type = kGpuCommandDraw;

    GpuDrawArguments *draw = &command.arguments.draw;

    draw->pipeline                 = pipeline;
    draw->texture[0]               = texture0;
    draw->sampler[0]               = sampler0;
    draw->texture[1]               = texture1;
    draw->sampler[1]               = sampler1;
    draw->base_vertex = pending_base_;
    draw->index_first = (int32_t)dynamic_indices_.size();

    if (index_source == kGpuIndexSourceDynamic)
        index_count = AppendDynamicIndices(shape, count, 0);

    draw->vertex_count             = count;
    draw->index_count              = index_count;
    draw->vertex_parameter_index   = vertex_parameters;
    draw->fragment_parameter_index = fragment_parameters;
    draw->index_source             = index_source;
    draw->mergeable                = mergeable;

    commands_.push_back(command);
}

void GpuImmediate::RecordMovieDraw(SDL_GPUTexture *luma, SDL_GPUTexture *chroma_blue, SDL_GPUTexture *chroma_red,
                                   SDL_GPUSampler *sampler, const float plane_scales[4])
{
    if (!luma || !chroma_blue || !chroma_red || !sampler)
        return;

    GpuCommand command;

    command.type = kGpuCommandMovie;

    GpuMovieArguments *movie = &command.arguments.movie;

    movie->texture[0] = luma;
    movie->texture[1] = chroma_blue;
    movie->texture[2] = chroma_red;
    movie->sampler     = sampler;
    movie->base_vertex = pending_base_;

    movie->mvp = HMM_MulM4(matrix_stack_[kGpuMatrixModeProjection][matrix_top_[kGpuMatrixModeProjection]],
                           matrix_stack_[kGpuMatrixModeModelView][matrix_top_[kGpuMatrixModeModelView]]);

    for (int32_t i = 0; i < 4; i++)
        movie->plane_scales[i] = plane_scales[i];

    commands_.push_back(command);
}

void GpuImmediate::Draw(GLuint shape, const RendererVertex *vertices, int32_t count)
{
    if (count <= 0)
        return;

    RendererVertex *destination = ReserveVertices(count);

    memcpy(destination, vertices, (size_t)count * sizeof(RendererVertex));

    RecordDraw(shape, count);
}

bool GpuImmediate::EnsureVertexCapacity(size_t bytes)
{
    if (vertex_buffer_ && vertex_buffer_capacity_ >= bytes)
        return true;

    size_t capacity = vertex_buffer_capacity_ ? vertex_buffer_capacity_ : kGpuInitialVertexCapacity;

    while (capacity < bytes)
        capacity *= 2;

    if (vertex_buffer_)
        SDL_ReleaseGPUBuffer(device_, vertex_buffer_);

    if (vertex_transfer_buffer_)
        SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer_buffer_);

    vertex_buffer_          = nullptr;
    vertex_transfer_buffer_ = nullptr;
    vertex_buffer_capacity_ = 0;

    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size  = (uint32_t)capacity;

    vertex_buffer_ = SDL_CreateGPUBuffer(device_, &buffer_info);

    if (!vertex_buffer_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUBuffer (vertex) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)capacity;

    vertex_transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);

    if (!vertex_transfer_buffer_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUTransferBuffer (vertex) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
        vertex_buffer_ = nullptr;
        return false;
    }

    vertex_buffer_capacity_ = capacity;

    return true;
}

int32_t GpuImmediate::AppendDynamicIndices(GLuint shape, int32_t count, int32_t rebase)
{
    size_t start = dynamic_indices_.size();

    switch (shape)
    {
    case GL_QUADS:
        for (int32_t q = 0; q + 4 <= count; q += 4)
        {
            uint16_t b = (uint16_t)(rebase + q);

            dynamic_indices_.push_back(b);
            dynamic_indices_.push_back((uint16_t)(b + 1));
            dynamic_indices_.push_back((uint16_t)(b + 2));
            dynamic_indices_.push_back(b);
            dynamic_indices_.push_back((uint16_t)(b + 2));
            dynamic_indices_.push_back((uint16_t)(b + 3));
        }
        break;

    case GL_TRIANGLES:
        for (int32_t t = 0, total = (count / 3) * 3; t < total; t++)
            dynamic_indices_.push_back((uint16_t)(rebase + t));
        break;

    case GL_POLYGON:
    case GL_TRIANGLE_FAN:
        for (int32_t t = 0; t + 2 < count; t++)
        {
            dynamic_indices_.push_back((uint16_t)rebase);
            dynamic_indices_.push_back((uint16_t)(rebase + t + 1));
            dynamic_indices_.push_back((uint16_t)(rebase + t + 2));
        }
        break;

    case GL_QUAD_STRIP:
    case GL_TRIANGLE_STRIP:
        for (int32_t t = 0; t + 2 < count; t++)
        {
            if (t & 1)
            {
                dynamic_indices_.push_back((uint16_t)(rebase + t + 1));
                dynamic_indices_.push_back((uint16_t)(rebase + t));
                dynamic_indices_.push_back((uint16_t)(rebase + t + 2));
            }
            else
            {
                dynamic_indices_.push_back((uint16_t)(rebase + t));
                dynamic_indices_.push_back((uint16_t)(rebase + t + 1));
                dynamic_indices_.push_back((uint16_t)(rebase + t + 2));
            }
        }
        break;

    default:
        break;
    }

    return (int32_t)(dynamic_indices_.size() - start);
}

bool GpuImmediate::EnsureIndexCapacity(size_t bytes)
{
    if (bytes <= dynamic_index_capacity_ && dynamic_index_buffer_)
        return true;

    size_t capacity = dynamic_index_capacity_ ? dynamic_index_capacity_ : 65536;

    while (capacity < bytes)
        capacity *= 2;

    if (dynamic_index_buffer_)
        SDL_ReleaseGPUBuffer(device_, dynamic_index_buffer_);

    if (dynamic_index_transfer_buffer_)
        SDL_ReleaseGPUTransferBuffer(device_, dynamic_index_transfer_buffer_);

    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    buffer_info.size  = (uint32_t)capacity;

    dynamic_index_buffer_ = SDL_CreateGPUBuffer(device_, &buffer_info);

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)capacity;

    dynamic_index_transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);

    if (!dynamic_index_buffer_ || !dynamic_index_transfer_buffer_)
    {
        LogPrint("GpuImmediate: dynamic index buffer allocation failed: %s\n", SDL_GetError());
        dynamic_index_capacity_ = 0;
        return false;
    }

    dynamic_index_capacity_ = capacity;

    return true;
}

void GpuImmediate::UploadIndices()
{
    if (dynamic_indices_.empty())
        return;

    size_t bytes = dynamic_indices_.size() * sizeof(uint16_t);

    if (!EnsureIndexCapacity(bytes))
        return;

    void *mapped = SDL_MapGPUTransferBuffer(device_, dynamic_index_transfer_buffer_, true);

    if (!mapped)
    {
        LogPrint("GpuImmediate: SDL_MapGPUTransferBuffer (dynamic index) failed: %s\n", SDL_GetError());
        return;
    }

    memcpy(mapped, dynamic_indices_.data(), bytes);

    SDL_UnmapGPUTransferBuffer(device_, dynamic_index_transfer_buffer_);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(gpu_device.CommandBuffer());

    SDL_GPUTransferBufferLocation source;
    source.transfer_buffer = dynamic_index_transfer_buffer_;
    source.offset          = 0;

    SDL_GPUBufferRegion destination;
    destination.buffer = dynamic_index_buffer_;
    destination.offset = 0;
    destination.size   = (uint32_t)bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

    SDL_EndGPUCopyPass(copy_pass);

    uploaded_bytes_ += bytes;
}

void GpuImmediate::UploadVertices()
{
    if (vertex_count_ == 0)
        return;

    size_t bytes = (size_t)vertex_count_ * sizeof(RendererVertex);

    if (!EnsureVertexCapacity(bytes))
        return;

    void *mapped = SDL_MapGPUTransferBuffer(device_, vertex_transfer_buffer_, true);

    if (!mapped)
    {
        LogPrint("GpuImmediate: SDL_MapGPUTransferBuffer (vertex) failed: %s\n", SDL_GetError());
        return;
    }

    memcpy(mapped, vertices_.data(), bytes);

    SDL_UnmapGPUTransferBuffer(device_, vertex_transfer_buffer_);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(gpu_device.CommandBuffer());

    SDL_GPUTransferBufferLocation source;
    source.transfer_buffer = vertex_transfer_buffer_;
    source.offset          = 0;

    SDL_GPUBufferRegion destination;
    destination.buffer = vertex_buffer_;
    destination.offset = 0;
    destination.size   = (uint32_t)bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

    SDL_EndGPUCopyPass(copy_pass);

    uploaded_bytes_ = bytes;
}

void GpuImmediate::ApplyPassState()
{
    SDL_GPURenderPass *pass = gpu_device.RenderPass();

    bound_pipeline_                 = nullptr;
    bound_texture_[0]               = nullptr;
    bound_texture_[1]               = nullptr;
    bound_sampler_[0]               = nullptr;
    bound_sampler_[1]               = nullptr;
    bound_index_buffer_             = nullptr;
    bound_vertex_parameter_index_   = -1;
    bound_fragment_parameter_index_ = -1;

    if (!pass)
        return;

    if (vertex_buffer_)
    {
        SDL_GPUBufferBinding binding;
        binding.buffer = vertex_buffer_;
        binding.offset = 0;

        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    }

    int32_t target_height = gpu_device.TargetHeight();

    if (viewport_set_)
    {
        SDL_GPUViewport viewport;
        viewport.x         = (float)current_viewport_.x;
        viewport.y         = (float)(target_height - current_viewport_.y - current_viewport_.height);
        viewport.w         = (float)current_viewport_.width;
        viewport.h         = (float)current_viewport_.height;
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;

        SDL_SetGPUViewport(pass, &viewport);
    }

    if (scissor_set_)
    {
        int32_t target_width = gpu_device.TargetWidth();

        int32_t left   = HMM_MAX(0, current_scissor_.x);
        int32_t bottom = HMM_MAX(0, current_scissor_.y);
        int32_t right  = HMM_MIN(target_width, current_scissor_.x + current_scissor_.width);
        int32_t top    = HMM_MIN(target_height, current_scissor_.y + current_scissor_.height);

        SDL_Rect rectangle;
        rectangle.x = left;
        rectangle.y = target_height - top;
        rectangle.w = HMM_MAX(0, right - left);
        rectangle.h = HMM_MAX(0, top - bottom);

        SDL_SetGPUScissor(pass, &rectangle);
    }
}

void GpuImmediate::Replay()
{
    draw_count_          = 0;
    pipeline_bind_count_ = 0;
    binding_count_       = 0;
    uniform_push_count_  = 0;
    uniform_bytes_       = 0;
    uploaded_bytes_      = 0;

    if (!gpu_device.FrameAcquired())
        return;

    UploadVertices();
    UploadIndices();

    gpu_device.BeginPass(kGpuLoadOperationClear, kGpuLoadOperationClear);

    ApplyPassState();

    for (size_t i = 0; i < commands_.size(); i++)
    {
        const GpuCommand *command = &commands_[i];

        if (command->type == kGpuCommandMovie)
        {
            const GpuMovieArguments *movie = &command->arguments.movie;

            SDL_GPURenderPass *pass = gpu_device.RenderPass();

            if (!pass)
            {
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationClear);
                ApplyPassState();
                pass = gpu_device.RenderPass();
            }

            if (!pass)
                continue;

            SDL_GPUGraphicsPipeline *movie_pipeline = GetMoviePipeline();

            SDL_BindGPUGraphicsPipeline(pass, movie_pipeline);
            bound_pipeline_ = movie_pipeline;
            pipeline_bind_count_++;

            SDL_GPUTextureSamplerBinding movie_bindings[3];
            for (int32_t i = 0; i < 3; i++)
            {
                movie_bindings[i].texture = movie->texture[i];
                movie_bindings[i].sampler = movie->sampler;
            }

            SDL_BindGPUFragmentSamplers(pass, 0, movie_bindings, 3);
            binding_count_++;

            bound_texture_[0] = nullptr;
            bound_texture_[1] = nullptr;
            bound_sampler_[0] = nullptr;
            bound_sampler_[1] = nullptr;

            GpuMovieVertexParameters movie_vertex;
            movie_vertex.mvp = movie->mvp;

            GpuMovieFragmentParameters movie_fragment;
            for (int32_t i = 0; i < 4; i++)
                movie_fragment.plane_scales[i] = movie->plane_scales[i];

            SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuVertexUniformSlot, &movie_vertex,
                                         (uint32_t)sizeof(movie_vertex));
            SDL_PushGPUFragmentUniformData(gpu_device.CommandBuffer(), kGpuFragmentUniformSlot, &movie_fragment,
                                           (uint32_t)sizeof(movie_fragment));
            uniform_push_count_ += 2;

            bound_vertex_parameter_index_   = -1;
            bound_fragment_parameter_index_ = -1;

            if (bound_index_buffer_ != quad_index_buffer_)
            {
                SDL_GPUBufferBinding movie_index_binding;
                movie_index_binding.buffer = quad_index_buffer_;
                movie_index_binding.offset = 0;

                SDL_BindGPUIndexBuffer(pass, &movie_index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                bound_index_buffer_ = quad_index_buffer_;
                binding_count_++;
            }

            SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, movie->base_vertex, 0);

            draw_count_++;

            continue;
        }

        if (command->type != kGpuCommandDraw)
        {
            if (command->type == kGpuCommandViewport)
            {
                current_viewport_ = command->arguments.rectangle;
                viewport_set_     = true;
            }
            else if (command->type == kGpuCommandScissor)
            {
                current_scissor_ = command->arguments.rectangle;
                scissor_set_     = true;
            }
            else
            {
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationClear);
            }

            ApplyPassState();
            continue;
        }

        SDL_GPURenderPass *pass = gpu_device.RenderPass();

        if (!pass || !vertex_buffer_)
            continue;

        const GpuDrawArguments *draw = &command->arguments.draw;

        if (bound_pipeline_ != draw->pipeline)
        {
            SDL_BindGPUGraphicsPipeline(pass, draw->pipeline);
            bound_pipeline_ = draw->pipeline;
            pipeline_bind_count_++;
        }

        if (bound_texture_[0] != draw->texture[0] || bound_sampler_[0] != draw->sampler[0] ||
            bound_texture_[1] != draw->texture[1] || bound_sampler_[1] != draw->sampler[1])
        {
            SDL_GPUTextureSamplerBinding bindings[2];

            bindings[0].texture = draw->texture[0];
            bindings[0].sampler = draw->sampler[0];
            bindings[1].texture = draw->texture[1];
            bindings[1].sampler = draw->sampler[1];

            SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);

            bound_texture_[0] = draw->texture[0];
            bound_sampler_[0] = draw->sampler[0];
            bound_texture_[1] = draw->texture[1];
            bound_sampler_[1] = draw->sampler[1];

            binding_count_++;
        }

        if (bound_vertex_parameter_index_ != draw->vertex_parameter_index)
        {
            SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuVertexUniformSlot,
                                         &vertex_parameters_[draw->vertex_parameter_index],
                                         (uint32_t)sizeof(GpuVertexParameters));

            bound_vertex_parameter_index_ = draw->vertex_parameter_index;
            uniform_push_count_++;
            uniform_bytes_ += sizeof(GpuVertexParameters);
        }

        if (bound_fragment_parameter_index_ != draw->fragment_parameter_index)
        {
            SDL_PushGPUFragmentUniformData(gpu_device.CommandBuffer(), kGpuFragmentUniformSlot,
                                           &fragment_parameters_[draw->fragment_parameter_index],
                                           (uint32_t)sizeof(GpuFragmentParameters));

            bound_fragment_parameter_index_ = draw->fragment_parameter_index;
            uniform_push_count_++;
            uniform_bytes_ += sizeof(GpuFragmentParameters);
        }

        if (draw->index_source != kGpuIndexSourceNone)
        {
            SDL_GPUBuffer *index_buffer = dynamic_index_buffer_;

            if (draw->index_source == kGpuIndexSourceQuad)
                index_buffer = quad_index_buffer_;
            else if (draw->index_source == kGpuIndexSourceFan)
                index_buffer = fan_index_buffer_;

            if (bound_index_buffer_ != index_buffer)
            {
                SDL_GPUBufferBinding binding;
                binding.buffer = index_buffer;
                binding.offset = 0;

                SDL_BindGPUIndexBuffer(pass, &binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                bound_index_buffer_ = index_buffer;
                binding_count_++;
            }

            SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)draw->index_count, 1, (uint32_t)draw->index_first,
                                         draw->base_vertex, 0);
        }
        else
        {
            SDL_DrawGPUPrimitives(pass, (uint32_t)draw->vertex_count, 1, (uint32_t)draw->base_vertex, 0);
        }

        draw_count_++;
    }
}

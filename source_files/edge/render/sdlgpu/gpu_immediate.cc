#include "gpu_immediate.h"

#include <math.h>
#include <string.h>

#include "epi.h"
#include "epi_math.h"
#include "epi_vector.h"
#include "gpu_device.h"
#include "gpu_images.h"
#include "gpu_lights.h"
#include "gpu_matrix.h"
#include "i_system.h"
#include "r_backend.h"
#include "r_lightgrid.h"

GpuImmediate gpu_immediate;

static constexpr int32_t kGpuQuadIndexTotal = kGpuMaximumQuads * 6;
static constexpr int32_t kGpuFanIndexTotal  = (kGpuMaximumFanVertices - 2) * 3;

static constexpr size_t kGpuInitialVertexCapacity = 64 * 1024 * sizeof(RendererVertex);

static void ResolveSkyCubeBinding(SDL_GPUTexture **texture, SDL_GPUSampler **sampler)
{
    if (*texture && *sampler)
        return;

    const GpuImage *cube = GetDefaultGpuCubemap(gpu_device.Handle());

    *texture = cube ? cube->texture : nullptr;
    *sampler = cube ? cube->sampler : nullptr;
}

static void ResolveColorLookupBinding(SDL_GPUTexture **texture, SDL_GPUSampler **sampler)
{
    if (*texture && *sampler)
        return;

    const GpuImage *volume = GetDefaultGpuVolume(gpu_device.Handle());

    *texture = volume ? volume->texture : nullptr;
    *sampler = volume ? volume->sampler : nullptr;
}

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

    for (int32_t i = 0; i < kGpuMatrixModeTotal; i++)
    {
        matrix_top_[i]      = 0;
        matrix_stack_[i][0] = epi::IdentityMatrix();
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

    for (size_t i = 0; i < model_meshes_.size(); i++)
        DeleteModelMesh((uint32_t)(i + 1));

    model_meshes_.clear();

    for (size_t i = 0; i < static_buffers_.size(); i++)
    {
        if (static_buffers_[i])
            SDL_ReleaseGPUBuffer(device, static_buffers_[i]);
    }

    static_buffers_.clear();

    if (sprite_buffer_)
    {
        SDL_ReleaseGPUBuffer(device, sprite_buffer_);
        sprite_buffer_ = nullptr;
    }

    if (sprite_transfer_buffer_)
    {
        SDL_ReleaseGPUTransferBuffer(device, sprite_transfer_buffer_);
        sprite_transfer_buffer_ = nullptr;
    }

    sprite_buffer_capacity_ = 0;

    if (static_transfer_buffer_)
    {
        SDL_ReleaseGPUTransferBuffer(device, static_transfer_buffer_);
        static_transfer_buffer_ = nullptr;
    }

    static_transfer_capacity_ = 0;

    for (size_t i = 0; i < deleted_static_buffers_.size(); i++)
        SDL_ReleaseGPUBuffer(device, deleted_static_buffers_[i]);

    deleted_static_buffers_.clear();

    bound_vertex_buffer_ = nullptr;

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

    sprite_instance_count_ = 0;
    sprite_light_tables_.clear();
    sprite_light_table_sources_.clear();
    dynamic_indices_.clear();
    commands_.clear();

    FlushDeletedStaticBuffers();
    vertex_parameters_.clear();
    fragment_parameters_.clear();

    model_vertex_parameters_.clear();
    model_fragment_parameters_.clear();

    for (int32_t i = 0; i < kGpuMatrixModeTotal; i++)
    {
        matrix_top_[i]      = 0;
        matrix_stack_[i][0] = epi::IdentityMatrix();
    }

    current_matrix_mode_ = kGpuMatrixModeModelView;

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

    oit_pipeline_ = false;

    pending_base_  = 0;
    pending_count_ = 0;

    viewport_set_ = false;
    scissor_set_  = false;
}

void GpuImmediate::LoadIdentity()
{
    matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]] = epi::IdentityMatrix();
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

void GpuImmediate::LoadMatrix(const epi::Mat4 &matrix)
{
    matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]] = matrix;
    MarkMatrixDirty();
}

void GpuImmediate::MultiplyMatrix(const epi::Mat4 &matrix)
{
    epi::Mat4 &current = matrix_stack_[current_matrix_mode_][matrix_top_[current_matrix_mode_]];

    current = epi::MultiplyMatrices(current, matrix);

    MarkMatrixDirty();
}

void GpuImmediate::Translate(float x, float y, float z)
{
    MultiplyMatrix(epi::TranslationMatrix(epi::Vec3{x, y, z}));
}

void GpuImmediate::Rotate(float radians, float x, float y, float z)
{
    if (sqrtf(x * x + y * y + z * z) < 1.0e-4f)
        return;

    MultiplyMatrix(epi::RotationMatrix(radians, epi::Vec3{x, y, z}));
}

void GpuImmediate::Scale(float x, float y, float z)
{
    MultiplyMatrix(epi::ScaleMatrix(epi::Vec3{x, y, z}));
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

void GpuImmediate::SetStencilReference(uint8_t reference)
{
    stencil_reference_ = reference;
}

void GpuImmediate::ClearStencil()
{
    GpuCommand command;

    command.type = kGpuCommandClearStencil;

    commands_.push_back(command);
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

static SDL_GPUBuffer *CreateStaticModelBuffer(SDL_GPUDevice *device, SDL_GPUBufferUsageFlags usage, const void *data,
                                              size_t bytes, const char *what);


uint32_t GpuImmediate::CreateStaticBuffer(const RendererVertex *vertices, int count, int capacity)
{
    if (!vertices || count <= 0)
        return 0;

    if (capacity < count)
        capacity = count;

    return CreateStaticBytes(vertices, (size_t)count * sizeof(RendererVertex),
                             (size_t)capacity * sizeof(RendererVertex));
}

uint32_t GpuImmediate::CreateStaticBytes(const void *data, size_t bytes, size_t capacity)
{
    if (!data || bytes == 0 || !device_)
        return 0;

    if (capacity < bytes)
        capacity = bytes;

    SDL_GPUBuffer *buffer = nullptr;

    if (capacity == bytes)
    {
        buffer = CreateStaticModelBuffer(device_, SDL_GPU_BUFFERUSAGE_VERTEX, data, bytes, "static mesh");
    }
    else
    {
        SDL_GPUBufferCreateInfo buffer_info;
        EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

        buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buffer_info.size  = (uint32_t)capacity;

        buffer = SDL_CreateGPUBuffer(device_, &buffer_info);

        if (buffer)
            QueueStaticUpload(buffer, 0, data, bytes);
    }

    if (!buffer)
        return 0;

    for (size_t i = 0; i < static_buffers_.size(); i++)
    {
        if (!static_buffers_[i])
        {
            static_buffers_[i] = buffer;
            return (uint32_t)(i + 1);
        }
    }

    static_buffers_.push_back(buffer);

    return (uint32_t)static_buffers_.size();
}

void GpuImmediate::UpdateStaticBuffer(uint32_t handle, int first, const RendererVertex *vertices, int count)
{
    if (!vertices || count <= 0 || first < 0)
        return;

    UpdateStaticBytes(handle, (size_t)first * sizeof(RendererVertex), vertices, (size_t)count * sizeof(RendererVertex));
}

void GpuImmediate::UpdateStaticBytes(uint32_t handle, size_t offset, const void *data, size_t bytes)
{
    if (handle == 0 || handle > static_buffers_.size() || !data || bytes == 0 || !device_)
        return;

    SDL_GPUBuffer *buffer = static_buffers_[handle - 1];

    if (!buffer)
        return;

    QueueStaticUpload(buffer, (uint32_t)offset, data, bytes);
}

void GpuImmediate::QueueStaticUpload(SDL_GPUBuffer *buffer, uint32_t offset, const void *data, size_t bytes)
{
    PendingStaticUpload upload;

    upload.buffer      = buffer;
    upload.offset      = offset;
    upload.data_offset = static_upload_data_.size();
    upload.bytes       = bytes;

    const uint8_t *source = (const uint8_t *)data;

    static_upload_data_.insert(static_upload_data_.end(), source, source + bytes);
    static_uploads_.push_back(upload);
}

bool GpuImmediate::RecordFrameStaticUploads()
{
    SDL_GPUCommandBuffer *command_buffer = gpu_device.CommandBuffer();

    if (!command_buffer || gpu_device.RenderPass())
        return false;

    size_t bytes = static_upload_data_.size();

    if (bytes > static_transfer_capacity_)
    {
        if (static_transfer_buffer_)
            SDL_ReleaseGPUTransferBuffer(device_, static_transfer_buffer_);

        size_t capacity = epi::Max(bytes, epi::Max(static_transfer_capacity_ * 2, (size_t)65536));

        SDL_GPUTransferBufferCreateInfo transfer_info;
        EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size  = (uint32_t)capacity;

        static_transfer_buffer_   = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
        static_transfer_capacity_ = static_transfer_buffer_ ? capacity : 0;

        if (!static_transfer_buffer_)
            return false;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device_, static_transfer_buffer_, true);

    if (!mapped)
        return false;

    memcpy(mapped, static_upload_data_.data(), bytes);

    SDL_UnmapGPUTransferBuffer(device_, static_transfer_buffer_);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    for (size_t i = 0; i < static_uploads_.size(); i++)
    {
        const PendingStaticUpload &upload = static_uploads_[i];

        SDL_GPUTransferBufferLocation source;
        SDL_GPUBufferRegion           destination;

        source.transfer_buffer = static_transfer_buffer_;
        source.offset          = (uint32_t)upload.data_offset;

        destination.buffer = upload.buffer;
        destination.offset = upload.offset;
        destination.size   = (uint32_t)upload.bytes;

        SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
    }

    SDL_EndGPUCopyPass(copy_pass);

    return true;
}

void GpuImmediate::FlushStaticUploads()
{
    if (static_uploads_.empty() || !device_)
    {
        static_uploads_.clear();
        static_upload_data_.clear();
        return;
    }

    if (RecordFrameStaticUploads())
    {
        static_uploads_.clear();
        static_upload_data_.clear();
        return;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)static_upload_data_.size();

    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);

    void *mapped = transfer ? SDL_MapGPUTransferBuffer(device_, transfer, false) : nullptr;

    SDL_GPUCommandBuffer *command_buffer = mapped ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;

    if (!command_buffer)
    {
        LogPrint("GpuImmediate: static mesh upload failed: %s\n", SDL_GetError());

        if (mapped)
            SDL_UnmapGPUTransferBuffer(device_, transfer);

        if (transfer)
            SDL_ReleaseGPUTransferBuffer(device_, transfer);

        static_uploads_.clear();
        static_upload_data_.clear();
        return;
    }

    memcpy(mapped, static_upload_data_.data(), static_upload_data_.size());

    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    for (size_t i = 0; i < static_uploads_.size(); i++)
    {
        const PendingStaticUpload &upload = static_uploads_[i];

        SDL_GPUTransferBufferLocation source;
        SDL_GPUBufferRegion           destination;

        source.transfer_buffer = transfer;
        source.offset          = (uint32_t)upload.data_offset;

        destination.buffer = upload.buffer;
        destination.offset = upload.offset;
        destination.size   = (uint32_t)upload.bytes;

        SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
    }

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);

    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    static_uploads_.clear();
    static_upload_data_.clear();
}

void GpuImmediate::DeleteStaticBuffer(uint32_t handle)
{
    if (handle == 0 || handle > static_buffers_.size())
        return;

    SDL_GPUBuffer *buffer = static_buffers_[handle - 1];

    if (!buffer)
        return;

    if (bound_vertex_buffer_ == buffer)
        bound_vertex_buffer_ = nullptr;

    size_t keep = 0;

    for (size_t i = 0; i < static_uploads_.size(); i++)
    {
        if (static_uploads_[i].buffer != buffer)
            static_uploads_[keep++] = static_uploads_[i];
    }

    static_uploads_.resize(keep);

    deleted_static_buffers_.push_back(buffer);

    static_buffers_[handle - 1] = nullptr;
}

void GpuImmediate::FlushDeletedStaticBuffers()
{
    if (device_)
    {
        for (size_t i = 0; i < deleted_static_buffers_.size(); i++)
            SDL_ReleaseGPUBuffer(device_, deleted_static_buffers_[i]);
    }

    deleted_static_buffers_.clear();
}

void GpuImmediate::DrawStatic(uint32_t handle, int32_t first, int32_t count)
{
    if (handle == 0 || handle > static_buffers_.size() || count < 3)
        return;

    SDL_GPUBuffer *buffer = static_buffers_[handle - 1];

    if (!buffer)
        return;

    count -= count % 3;

    SDL_GPUGraphicsPipeline *pipeline = SelectWorldPipeline(kGpuPrimitiveTriangleList);

    GpuCommand command;

    command.type = kGpuCommandDraw;

    GpuDrawArguments *draw = &command.arguments.draw;

    draw->pipeline                 = pipeline;
    draw->texture[0]               = texturing_enabled_ ? current_texture_[0] : default_texture_;
    draw->sampler[0]               = texturing_enabled_ ? current_sampler_[0] : default_sampler_;
    draw->texture[1]               = texturing_enabled_ ? current_texture_[1] : default_texture_;
    draw->sampler[1]               = texturing_enabled_ ? current_sampler_[1] : default_sampler_;
    draw->texture[2]               = current_sky_cube_texture_;
    draw->sampler[2]               = current_sky_cube_sampler_;
    ResolveSkyCubeBinding(&draw->texture[2], &draw->sampler[2]);
    draw->texture[3] = current_color_lookup_texture_;
    draw->sampler[3] = current_color_lookup_sampler_;
    ResolveColorLookupBinding(&draw->texture[3], &draw->sampler[3]);
    draw->base_vertex              = first;
    draw->vertex_count             = count;
    draw->index_first              = 0;
    draw->index_count              = 0;
    draw->vertex_parameter_index   = CurrentVertexParameters();
    draw->fragment_parameter_index = CurrentFragmentParameters();
    draw->index_source             = kGpuIndexSourceNone;
    draw->stencil_reference        = stencil_reference_;
    draw->mergeable                = false;
    draw->vertex_buffer            = buffer;

    commands_.push_back(command);
}

SpriteInstance *GpuImmediate::ReserveSpriteInstances(int32_t count, int32_t *first)
{
    EPI_ASSERT(count > 0);

    size_t required = (size_t)sprite_instance_count_ + (size_t)count;

    if (required > sprite_instances_.size())
    {
        size_t capacity = sprite_instances_.empty() ? (size_t)4096 : sprite_instances_.size();

        while (capacity < required)
            capacity *= 2;

        sprite_instances_.resize(capacity);
    }

    *first = sprite_instance_count_;

    sprite_instance_count_ += count;

    return sprite_instances_.data() + *first;
}

int32_t GpuImmediate::SpriteLightTableIndex(const SpriteLightTable *light_table)
{
    for (size_t i = 0; i < sprite_light_table_sources_.size(); i++)
    {
        if (sprite_light_table_sources_[i] == light_table)
            return (int32_t)i;
    }

    SpriteLightTable table;

    if (light_table)
        table = *light_table;
    else
        EPI_CLEAR_MEMORY(&table, SpriteLightTable, 1);

    sprite_light_tables_.push_back(table);
    sprite_light_table_sources_.push_back(light_table);

    return (int32_t)sprite_light_tables_.size() - 1;
}

void GpuImmediate::DrawSprites(int32_t first, int32_t count, const SpriteLightTable *light_table, uint32_t buffer)
{
    if (count <= 0)
        return;

    SDL_GPUBuffer *instance_buffer = nullptr;

    if (buffer)
    {
        if (buffer > static_buffers_.size() || !static_buffers_[buffer - 1])
            return;

        instance_buffer = static_buffers_[buffer - 1];
    }

    SDL_GPUGraphicsPipeline *pipeline = oit_pipeline_
                                            ? GetSpriteOitPipeline(pipeline_flags_)
                                            : GetSpritePipeline(pipeline_flags_, source_blend_, destination_blend_);

    GpuCommand command;

    command.type = kGpuCommandSpriteDraw;

    GpuSpriteDrawArguments *draw = &command.arguments.sprite_draw;

    draw->pipeline   = pipeline;
    draw->texture[0] = texturing_enabled_ ? current_texture_[0] : default_texture_;
    draw->sampler[0] = texturing_enabled_ ? current_sampler_[0] : default_sampler_;
    draw->texture[1] = texturing_enabled_ ? current_texture_[1] : default_texture_;
    draw->sampler[1] = texturing_enabled_ ? current_sampler_[1] : default_sampler_;
    draw->texture[2] = current_sky_cube_texture_;
    draw->sampler[2] = current_sky_cube_sampler_;
    ResolveSkyCubeBinding(&draw->texture[2], &draw->sampler[2]);
    draw->texture[3] = current_color_lookup_texture_;
    draw->sampler[3] = current_color_lookup_sampler_;
    ResolveColorLookupBinding(&draw->texture[3], &draw->sampler[3]);

    draw->instance_first           = first;
    draw->instance_count           = count;
    draw->vertex_parameter_index   = CurrentVertexParameters();
    draw->fragment_parameter_index = CurrentFragmentParameters();
    draw->light_table_index        = SpriteLightTableIndex(light_table);
    draw->stencil_reference        = stencil_reference_;
    draw->buffer                   = instance_buffer;

    commands_.push_back(command);
}

void GpuImmediate::SetViewTint(float r, float g, float b)
{
    if (epi::AlmostEquals(view_tint_[0], r) && epi::AlmostEquals(view_tint_[1], g) &&
        epi::AlmostEquals(view_tint_[2], b))
        return;

    view_tint_[0] = r;
    view_tint_[1] = g;
    view_tint_[2] = b;

    vertex_parameters_dirty_ = true;
}

void GpuImmediate::SetTextureOffset(const epi::Vec2 &offset)
{
    if (epi::AlmostEquals(texture_offset_[0], offset.x) && epi::AlmostEquals(texture_offset_[1], offset.y))
        return;

    texture_offset_[0] = offset.x;
    texture_offset_[1] = offset.y;

    vertex_parameters_dirty_ = true;
}

void GpuImmediate::SetLightRowOffset(float offset)
{
    if (epi::AlmostEquals(light_row_offset_, offset))
        return;

    light_row_offset_ = offset;

    vertex_parameters_dirty_ = true;
}

void GpuImmediate::SetSpriteView(const epi::Vec4 view[2])
{
    if (!memcmp(sprite_view_, view, sizeof(sprite_view_)))
        return;

    sprite_view_[0] = view[0];
    sprite_view_[1] = view[1];

    vertex_parameters_dirty_ = true;
}

void GpuImmediate::SetLiquid(const epi::Vec4 &liquid)
{
    float *current = current_fragment_parameters_.liquid;

    if (epi::AlmostEquals(current[0], liquid.x) && epi::AlmostEquals(current[1], liquid.y) &&
        epi::AlmostEquals(current[2], liquid.z) && epi::AlmostEquals(current[3], liquid.w))
        return;

    current[0] = liquid.x;
    current[1] = liquid.y;
    current[2] = liquid.z;
    current[3] = liquid.w;

    fragment_parameters_dirty_ = true;
}

bool GpuImmediate::SetColorLookup(int slot)
{
    const GpuImage *image = nullptr;

    if (slot > 0 && slot < kColorLookupMaximum && color_lookup_ids_[slot] != 0)
        image = GetGpuImage(color_lookup_ids_[slot]);

    current_color_lookup_texture_ = image ? image->texture : nullptr;
    current_color_lookup_sampler_ = image ? image->sampler : nullptr;

    float enabled = image ? 1.0f : 0.0f;

    if (!epi::AlmostEquals(current_fragment_parameters_.color_lookup[0], enabled))
    {
        current_fragment_parameters_.color_lookup[0] = enabled;
        fragment_parameters_dirty_                   = true;
    }

    return image != nullptr;
}

void GpuImmediate::SetWhiten(bool enabled)
{
    bool current = (current_fragment_parameters_.flags & kGpuFragmentFlagWhiten) != 0;

    if (current == enabled)
        return;

    if (enabled)
        current_fragment_parameters_.flags |= kGpuFragmentFlagWhiten;
    else
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagWhiten;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetBlur(const epi::Vec4 &blur)
{
    float *current = current_fragment_parameters_.blur;

    if (epi::AlmostEquals(current[0], blur.x) && epi::AlmostEquals(current[2], blur.z) &&
        epi::AlmostEquals(current[3], blur.w))
        return;

    current[0] = blur.x;
    current[1] = blur.y;
    current[2] = blur.z;
    current[3] = blur.w;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::UploadColorLookup(int slot, const uint8_t *pixels)
{
    if (slot <= 0 || slot >= kColorLookupMaximum || !pixels)
        return;

    if (color_lookup_ids_[slot] == 0)
        color_lookup_ids_[slot] = AllocateGpuCubemapId();

    CreateGpuVolume(device_, color_lookup_ids_[slot], kColorLookupSize, pixels);
}

void GpuImmediate::SetLightDepth(bool enabled)
{
    if (enabled == light_depth_enabled_)
        return;

    light_depth_enabled_ = enabled;

    vertex_parameters_dirty_ = true;
}

void GpuImmediate::SetSkyPass(const SkyPassInfo *sky_pass)
{
    bool enabled = (sky_pass != nullptr);

    if (!enabled && !sky_pass_enabled_)
        return;

    sky_pass_enabled_ = enabled;

    if (enabled)
    {
        sky_pass_info_ = *sky_pass;

        current_fragment_parameters_.flags |= kGpuFragmentFlagSkyPass;

        current_fragment_parameters_.sky_inverse_projection = sky_pass->inverse_projection;
        current_fragment_parameters_.sky_inverse_view       = sky_pass->inverse_view;

        float sky_scale_x = render_backend->ActiveScaleX();
        float sky_scale_y = render_backend->ActiveScaleY();

        float sky_target_height =
            render_backend->RenderTargetActive() ? (float)gpu_device.WorldHeight() : (float)gpu_device.TargetHeight();

        current_fragment_parameters_.sky_viewport[0] = sky_pass->viewport_origin.x * sky_scale_x;
        current_fragment_parameters_.sky_viewport[1] = sky_target_height - sky_pass->viewport_origin.y * sky_scale_y;
        current_fragment_parameters_.sky_viewport[2] = sky_pass->viewport_size.x * sky_scale_x;
        current_fragment_parameters_.sky_viewport[3] = -sky_pass->viewport_size.y * sky_scale_y;

        current_fragment_parameters_.sky_stretch_mode       = (float)sky_pass->stretch_mode;
        current_fragment_parameters_.sky_u_scale            = sky_pass->u_scale;
        current_fragment_parameters_.sky_ty                 = sky_pass->ty;
        current_fragment_parameters_.sky_u_offset           = sky_pass->u_offset;
        current_fragment_parameters_.sky_v_offset           = sky_pass->v_offset;
        current_fragment_parameters_.sky_vertical_fov_slope = sky_pass->vertical_fov_slope;
        current_fragment_parameters_.sky_horizon_shift      = sky_pass->horizon_shift;
        current_fragment_parameters_.sky_is_box            = sky_pass->is_box ? 1.0f : 0.0f;

        const GpuImage *cube = sky_pass->cube_texture ? GetGpuImage(sky_pass->cube_texture) : nullptr;

        if (!cube)
            cube = GetDefaultGpuCubemap(gpu_device.Handle());

        current_sky_cube_texture_ = cube ? cube->texture : nullptr;
        current_sky_cube_sampler_ = cube ? cube->sampler : nullptr;
    }
    else
    {
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagSkyPass;

        const GpuImage *cube = GetDefaultGpuCubemap(gpu_device.Handle());

        current_sky_cube_texture_ = cube ? cube->texture : nullptr;
        current_sky_cube_sampler_ = cube ? cube->sampler : nullptr;
    }

    fragment_parameters_dirty_ = true;
    vertex_parameters_dirty_   = true;
}

void GpuImmediate::SetOitPipeline(bool enabled)
{
    oit_pipeline_ = enabled;
}

void GpuImmediate::SetOitComposite(bool enabled)
{
    bool current = (current_fragment_parameters_.flags & kGpuFragmentFlagOitComposite) != 0;

    if (current == enabled)
        return;

    if (enabled)
        current_fragment_parameters_.flags |= kGpuFragmentFlagOitComposite;
    else
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagOitComposite;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetSkipRGB(bool enabled)
{
    bool current = (current_fragment_parameters_.flags & kGpuFragmentFlagSkipRGB) != 0;

    if (current == enabled)
        return;

    if (enabled)
        current_fragment_parameters_.flags |= kGpuFragmentFlagSkipRGB;
    else
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagSkipRGB;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetWorldLit(bool enabled, int view_index)
{
    const GpuLightViewParameters *view = enabled ? GpuLightView(view_index) : nullptr;

    float wanted[8];

    if (view)
    {
        for (int i = 0; i < 4; i++)
        {
            wanted[i]     = view->light_view[i];
            wanted[4 + i] = view->light_range[i];
        }
    }
    else
    {
        for (int i = 0; i < 8; i++)
            wanted[i] = 0.0f;
    }

    bool changed = false;

    for (int i = 0; i < 4; i++)
    {
        if (!epi::AlmostEquals(current_fragment_parameters_.light_view[i], wanted[i]) ||
            !epi::AlmostEquals(current_fragment_parameters_.light_range[i], wanted[4 + i]))
            changed = true;
    }

    if (!changed)
        return;

    for (int i = 0; i < 4; i++)
    {
        current_fragment_parameters_.light_view[i]  = wanted[i];
        current_fragment_parameters_.light_range[i] = wanted[4 + i];
    }

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetGlowSet(int index)
{
    const LightGridGlowSet *set = LightGridGlowSetAt(index);

    float count = (set && set->count > 0) ? (float)set->count : 0.0f;

    if (epi::AlmostEquals(current_fragment_parameters_.glow_additive[3], count) && count == 0.0f)
        return;

    for (int i = 0; i < 2; i++)
    {
        bool live = set && i < set->count;

        for (int e = 0; e < 4; e++)
        {
            current_fragment_parameters_.glow_plane[i][e] = live ? set->glows[i].plane[e] : 0.0f;
            current_fragment_parameters_.glow_color[i][e] = 0.0f;
        }

        if (live)
        {
            current_fragment_parameters_.glow_color[i][0] = set->glows[i].color[0] / 255.0f;
            current_fragment_parameters_.glow_color[i][1] = set->glows[i].color[1] / 255.0f;
            current_fragment_parameters_.glow_color[i][2] = set->glows[i].color[2] / 255.0f;
            current_fragment_parameters_.glow_color[i][3] = set->glows[i].radius;
        }
        else
            current_fragment_parameters_.glow_color[i][3] = 1.0f;

        current_fragment_parameters_.glow_additive[i] = live ? set->glows[i].additive : 0.0f;
    }

    current_fragment_parameters_.glow_additive[3] = count;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::SetLightFalloff(bool enabled)
{
    bool current = (current_fragment_parameters_.flags & kGpuFragmentFlagLightFalloff) != 0;

    if (current == enabled)
        return;

    if (enabled)
        current_fragment_parameters_.flags |= kGpuFragmentFlagLightFalloff;
    else
        current_fragment_parameters_.flags &= ~kGpuFragmentFlagLightFalloff;

    fragment_parameters_dirty_ = true;
}

void GpuImmediate::Viewport(int32_t x, int32_t y, int32_t width, int32_t height)
{
    GpuCommand command;

    command.type                       = kGpuCommandViewport;
    command.arguments.rectangle.x      = render_backend->ScaleToRenderTargetX(x);
    command.arguments.rectangle.y      = render_backend->ScaleToRenderTargetY(y);
    command.arguments.rectangle.width  = render_backend->ScaleToRenderTargetX(width);
    command.arguments.rectangle.height = render_backend->ScaleToRenderTargetY(height);

    commands_.push_back(command);
}

void GpuImmediate::ScissorRect(int32_t x, int32_t y, int32_t width, int32_t height)
{
    GpuCommand command;

    command.type                       = kGpuCommandScissor;
    command.arguments.rectangle.x      = render_backend->ScaleToRenderTargetX(x);
    command.arguments.rectangle.y      = render_backend->ScaleToRenderTargetY(y);
    command.arguments.rectangle.width  = render_backend->ScaleToRenderTargetX(width);
    command.arguments.rectangle.height = render_backend->ScaleToRenderTargetY(height);

    commands_.push_back(command);
}

static bool IsDrawCommand(GpuCommandType type)
{
    return type == kGpuCommandDraw || type == kGpuCommandModelDraw || type == kGpuCommandSpriteDraw ||
           type == kGpuCommandLightDraw || type == kGpuCommandMovie;
}

bool GpuImmediate::HasDrawCommands() const
{
    for (size_t i = 0; i < commands_.size(); i++)
    {
        if (IsDrawCommand(commands_[i].type))
            return true;
    }

    return false;
}

void GpuImmediate::BeginWorldTarget(bool direct)
{
    GpuCommand command;

    command.type                     = kGpuCommandBeginWorldTarget;
    command.arguments.resolve.direct = direct;

    commands_.push_back(command);
}

void GpuImmediate::BeginOitTarget()
{
    GpuCommand command;

    command.type = kGpuCommandBeginOitTarget;

    oit_begin_command_ = commands_.size();

    commands_.push_back(command);
}

bool GpuImmediate::EndOitTarget()
{
    if (oit_begin_command_ < commands_.size() && commands_[oit_begin_command_].type == kGpuCommandBeginOitTarget)
    {
        bool drawn = false;

        for (size_t i = oit_begin_command_ + 1; i < commands_.size() && !drawn; i++)
            drawn = IsDrawCommand(commands_[i].type);

        if (!drawn)
        {
            commands_.erase(commands_.begin() + (ptrdiff_t)oit_begin_command_);
            return false;
        }
    }

    GpuCommand command;

    command.type = kGpuCommandEndOitTarget;

    commands_.push_back(command);

    return true;
}

void GpuImmediate::ResolveWorldTarget(const GpuResolveArguments &resolve)
{
    GpuCommand command;

    command.type              = kGpuCommandResolveWorldTarget;
    command.arguments.resolve = resolve;

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
    parameters.mvp = epi::MultiplyMatrices(
        matrix_stack_[kGpuMatrixModeProjection][matrix_top_[kGpuMatrixModeProjection]], parameters.mv);
    parameters.tm  = matrix_stack_[kGpuMatrixModeTexture][matrix_top_[kGpuMatrixModeTexture]];


    parameters.sky_pass      = sky_pass_enabled_ ? 1.0f : 0.0f;
    parameters.sky_fog_depth = sky_pass_info_.fog_depth;
    parameters.light_depth    = light_depth_enabled_ ? 1.0f : 0.0f;
    parameters.sky_geometry   = (sky_pass_enabled_ && sky_pass_info_.is_geometry) ? 1.0f : 0.0f;
    parameters.view_tint[0]   = view_tint_[0];
    parameters.view_tint[1]   = view_tint_[1];
    parameters.view_tint[2]   = view_tint_[2];
    parameters.view_tint[3]   = 1.0f;

    parameters.texture_offset[0] = texture_offset_[0];
    parameters.texture_offset[1] = texture_offset_[1];
    parameters.light_row_offset   = light_row_offset_;
    parameters.vertex_padding0    = 0.0f;

    parameters.sprite_view0[0] = sprite_view_[0].x;
    parameters.sprite_view0[1] = sprite_view_[0].y;
    parameters.sprite_view0[2] = sprite_view_[0].z;
    parameters.sprite_view0[3] = sprite_view_[0].w;
    parameters.sprite_view1[0] = sprite_view_[1].x;
    parameters.sprite_view1[1] = sprite_view_[1].y;
    parameters.sprite_view1[2] = sprite_view_[1].z;
    parameters.sprite_view1[3] = sprite_view_[1].w;

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

SDL_GPUGraphicsPipeline *GpuImmediate::SelectWorldPipeline(GpuPrimitiveType primitive)
{
    if (oit_pipeline_)
        return GetOitPipeline(pipeline_flags_, primitive);

    return GetPipeline(pipeline_flags_, source_blend_, destination_blend_, primitive);
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

    SDL_GPUGraphicsPipeline *pipeline = SelectWorldPipeline(primitive);

    SDL_GPUTexture *lookup_texture = current_color_lookup_texture_;
    SDL_GPUSampler *lookup_sampler = current_color_lookup_sampler_;

    ResolveColorLookupBinding(&lookup_texture, &lookup_sampler);

    if (mergeable && !commands_.empty())
    {
        GpuCommand *previous = &commands_.back();

        if (previous->type == kGpuCommandDraw)
        {
            GpuDrawArguments *draw = &previous->arguments.draw;

            if (draw->mergeable && draw->pipeline == pipeline && draw->index_source == index_source &&
                index_source == kGpuIndexSourceDynamic && draw->texture[0] == texture0 &&
                draw->sampler[0] == sampler0 && draw->texture[1] == texture1 && draw->sampler[1] == sampler1 &&
                draw->texture[3] == lookup_texture && draw->sampler[3] == lookup_sampler &&
                draw->vertex_parameter_index == vertex_parameters &&
                draw->fragment_parameter_index == fragment_parameters &&
                draw->stencil_reference == stencil_reference_ &&
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
    draw->texture[2]               = current_sky_cube_texture_;
    draw->sampler[2]               = current_sky_cube_sampler_;
    ResolveSkyCubeBinding(&draw->texture[2], &draw->sampler[2]);
    draw->texture[3] = current_color_lookup_texture_;
    draw->sampler[3] = current_color_lookup_sampler_;
    ResolveColorLookupBinding(&draw->texture[3], &draw->sampler[3]);
    draw->base_vertex = pending_base_;
    draw->index_first = (int32_t)dynamic_indices_.size();

    if (index_source == kGpuIndexSourceDynamic)
        index_count = AppendDynamicIndices(shape, count, 0);

    draw->vertex_count             = count;
    draw->index_count              = index_count;
    draw->vertex_parameter_index   = vertex_parameters;
    draw->fragment_parameter_index = fragment_parameters;
    draw->index_source             = index_source;
    draw->vertex_buffer            = nullptr;
    draw->stencil_reference        = stencil_reference_;
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

    movie->mvp = epi::MultiplyMatrices(matrix_stack_[kGpuMatrixModeProjection][matrix_top_[kGpuMatrixModeProjection]],
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

static SDL_GPUBuffer *CreateStaticModelBuffer(SDL_GPUDevice *device, SDL_GPUBufferUsageFlags usage, const void *data,
                                              size_t bytes, const char *what)
{
    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = usage;
    buffer_info.size  = (uint32_t)bytes;

    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, &buffer_info);

    if (!buffer)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUBuffer (%s) failed: %s\n", what, SDL_GetError());
        return nullptr;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)bytes;

    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

    if (!transfer)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUTransferBuffer (%s) failed: %s\n", what, SDL_GetError());
        SDL_ReleaseGPUBuffer(device, buffer);
        return nullptr;
    }

    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);

    if (!mapped)
    {
        LogPrint("GpuImmediate: SDL_MapGPUTransferBuffer (%s) failed: %s\n", what, SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, buffer);
        return nullptr;
    }

    memcpy(mapped, data, bytes);

    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);

    if (!command_buffer)
    {
        LogPrint("GpuImmediate: SDL_AcquireGPUCommandBuffer (%s) failed: %s\n", what, SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUBuffer(device, buffer);
        return nullptr;
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    SDL_GPUTransferBufferLocation source;
    SDL_GPUBufferRegion           destination;

    source.transfer_buffer = transfer;
    source.offset          = 0;

    destination.buffer = buffer;
    destination.offset = 0;
    destination.size   = (uint32_t)bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);

    SDL_ReleaseGPUTransferBuffer(device, transfer);

    return buffer;
}

uint32_t GpuImmediate::CreateModelMesh(const ModelMeshData &data, const uint16_t *indices, int32_t index_count)
{
    if (!device_ || !data.frame_positions || !data.texture_coordinates || !indices)
        return 0;

    if (data.vertex_count <= 0 || data.frame_count <= 0 || index_count <= 0)
        return 0;

    GpuModelMesh mesh;
    EPI_CLEAR_MEMORY(&mesh, GpuModelMesh, 1);

    mesh.vertex_count = data.vertex_count;
    mesh.frame_count  = data.frame_count;

    size_t position_bytes = (size_t)data.vertex_count * (size_t)data.frame_count * 3 * sizeof(float);
    size_t texture_bytes  = (size_t)data.vertex_count * 2 * sizeof(float);
    size_t index_bytes    = (size_t)index_count * sizeof(uint16_t);

    mesh.position_buffer = CreateStaticModelBuffer(device_, SDL_GPU_BUFFERUSAGE_VERTEX, data.frame_positions,
                                                   position_bytes, "model positions");

    mesh.normal_buffer = data.frame_normals ? CreateStaticModelBuffer(device_, SDL_GPU_BUFFERUSAGE_VERTEX,
                                                                      data.frame_normals, position_bytes,
                                                                      "model normals")
                                            : nullptr;

    mesh.texture_coordinate_buffer = CreateStaticModelBuffer(device_, SDL_GPU_BUFFERUSAGE_VERTEX,
                                                             data.texture_coordinates, texture_bytes, "model texcoords");

    mesh.index_buffer =
        CreateStaticModelBuffer(device_, SDL_GPU_BUFFERUSAGE_INDEX, indices, index_bytes, "model indices");

    if (!mesh.position_buffer || !mesh.normal_buffer || !mesh.texture_coordinate_buffer || !mesh.index_buffer)
    {
        LogPrint("GpuImmediate: model mesh creation failed\n");

        model_meshes_.push_back(mesh);
        DeleteModelMesh((uint32_t)model_meshes_.size());

        return 0;
    }

    model_meshes_.push_back(mesh);

    return (uint32_t)model_meshes_.size();
}

void GpuImmediate::DeleteModelMesh(uint32_t handle)
{
    if (handle == 0 || handle > model_meshes_.size() || !device_)
        return;

    GpuModelMesh *mesh = &model_meshes_[handle - 1];

    if (mesh->position_buffer)
        SDL_ReleaseGPUBuffer(device_, mesh->position_buffer);

    if (mesh->normal_buffer)
        SDL_ReleaseGPUBuffer(device_, mesh->normal_buffer);

    if (mesh->texture_coordinate_buffer)
        SDL_ReleaseGPUBuffer(device_, mesh->texture_coordinate_buffer);

    if (mesh->index_buffer)
        SDL_ReleaseGPUBuffer(device_, mesh->index_buffer);

    EPI_CLEAR_MEMORY(mesh, GpuModelMesh, 1);
}

void GpuImmediate::RecordModelDraw(const ModelDrawInfo &info, const GpuModelVertexParameters &vertex_parameters,
                                   const GpuModelFragmentParameters &fragment_parameters)
{
    if (info.handle == 0 || info.handle > model_meshes_.size() || info.index_count <= 0)
        return;

    GpuModelMesh *mesh = &model_meshes_[info.handle - 1];

    if (!mesh->position_buffer || info.frame1 >= mesh->frame_count || info.frame2 >= mesh->frame_count)
        return;

    SDL_GPUGraphicsPipeline *pipeline = oit_pipeline_
                                            ? GetModelOitPipeline(pipeline_flags_)
                                            : GetModelPipeline(pipeline_flags_, source_blend_, destination_blend_);

    model_vertex_parameters_.push_back(vertex_parameters);
    model_fragment_parameters_.push_back(fragment_parameters);

    GpuCommand command;

    command.type = kGpuCommandModelDraw;

    GpuModelDrawArguments *draw = &command.arguments.model_draw;

    draw->pipeline = pipeline;

    draw->texture = texturing_enabled_ ? current_texture_[0] : default_texture_;
    draw->sampler = texturing_enabled_ ? current_sampler_[0] : default_sampler_;

    draw->lookup_texture = current_color_lookup_texture_;
    draw->lookup_sampler = current_color_lookup_sampler_;
    ResolveColorLookupBinding(&draw->lookup_texture, &draw->lookup_sampler);

    draw->position_buffer           = mesh->position_buffer;
    draw->normal_buffer             = mesh->normal_buffer;
    draw->texture_coordinate_buffer = mesh->texture_coordinate_buffer;
    draw->index_buffer              = mesh->index_buffer;

    uint32_t frame_stride = (uint32_t)((size_t)mesh->vertex_count * 3 * sizeof(float));
    uint32_t vertex_base  = (uint32_t)((size_t)info.first_vertex * 3 * sizeof(float));

    draw->position_frame1_offset = (uint32_t)info.frame1 * frame_stride + vertex_base;
    draw->position_frame2_offset = (uint32_t)info.frame2 * frame_stride + vertex_base;

    draw->normal_frame1_offset = draw->position_frame1_offset;
    draw->normal_frame2_offset = draw->position_frame2_offset;

    draw->texture_coordinate_offset = (uint32_t)((size_t)info.first_vertex * 2 * sizeof(float));

    draw->index_first = info.first_index;
    draw->index_count = info.index_count;

    draw->vertex_parameter_index   = (int32_t)model_vertex_parameters_.size() - 1;
    draw->fragment_parameter_index = (int32_t)model_fragment_parameters_.size() - 1;
    draw->light_table_index        = SpriteLightTableIndex(info.light_table);

    draw->stencil_reference = stencil_reference_;

    commands_.push_back(command);
}


void GpuImmediate::DrawIndexed(const RendererVertex *vertices, int32_t vertex_count, const uint16_t *indices,
                               int32_t index_count)
{
    if (!vertices || !indices || vertex_count <= 0 || index_count < 3)
        return;

    if (vertex_count > 65536)
        FatalError("GpuImmediate: indexed draw of %d vertices exceeds the 16-bit index range\n", vertex_count);

    index_count -= index_count % 3;

    if (index_count < 3)
        return;

    RendererVertex *destination = ReserveVertices(vertex_count);

    memcpy(destination, vertices, (size_t)vertex_count * sizeof(RendererVertex));

    int32_t vertex_parameters   = CurrentVertexParameters();
    int32_t fragment_parameters = CurrentFragmentParameters();

    SDL_GPUTexture *texture0 = texturing_enabled_ ? current_texture_[0] : default_texture_;
    SDL_GPUSampler *sampler0 = texturing_enabled_ ? current_sampler_[0] : default_sampler_;
    SDL_GPUTexture *texture1 = texturing_enabled_ ? current_texture_[1] : default_texture_;
    SDL_GPUSampler *sampler1 = texturing_enabled_ ? current_sampler_[1] : default_sampler_;

    SDL_GPUGraphicsPipeline *pipeline = SelectWorldPipeline(kGpuPrimitiveTriangleList);

    GpuCommand command;

    command.type = kGpuCommandDraw;

    GpuDrawArguments *draw = &command.arguments.draw;

    draw->pipeline   = pipeline;
    draw->texture[0] = texture0;
    draw->sampler[0] = sampler0;
    draw->texture[1] = texture1;
    draw->sampler[1] = sampler1;
    draw->texture[2] = current_sky_cube_texture_;
    draw->sampler[2] = current_sky_cube_sampler_;
    ResolveSkyCubeBinding(&draw->texture[2], &draw->sampler[2]);
    draw->texture[3] = current_color_lookup_texture_;
    draw->sampler[3] = current_color_lookup_sampler_;
    ResolveColorLookupBinding(&draw->texture[3], &draw->sampler[3]);

    draw->base_vertex = pending_base_;
    draw->index_first = (int32_t)dynamic_indices_.size();

    for (int32_t i = 0; i < index_count; i++)
        dynamic_indices_.push_back(indices[i]);

    draw->vertex_count             = vertex_count;
    draw->index_count              = index_count;
    draw->vertex_parameter_index   = vertex_parameters;
    draw->fragment_parameter_index = fragment_parameters;
    draw->index_source             = kGpuIndexSourceDynamic;
    draw->stencil_reference        = stencil_reference_;
    draw->mergeable                = false;

    commands_.push_back(command);
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

    bound_vertex_buffer_ = nullptr;

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

bool GpuImmediate::EnsureSpriteCapacity(size_t bytes)
{
    if (sprite_buffer_ && sprite_buffer_capacity_ >= bytes)
        return true;

    size_t capacity = sprite_buffer_capacity_ ? sprite_buffer_capacity_ : (size_t)(4096 * sizeof(SpriteInstance));

    while (capacity < bytes)
        capacity *= 2;

    if (sprite_buffer_)
        SDL_ReleaseGPUBuffer(device_, sprite_buffer_);

    if (sprite_transfer_buffer_)
        SDL_ReleaseGPUTransferBuffer(device_, sprite_transfer_buffer_);

    sprite_buffer_          = nullptr;
    sprite_transfer_buffer_ = nullptr;
    sprite_buffer_capacity_ = 0;

    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size  = (uint32_t)capacity;

    sprite_buffer_ = SDL_CreateGPUBuffer(device_, &buffer_info);

    bound_vertex_buffer_ = nullptr;

    if (!sprite_buffer_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUBuffer (sprite) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)capacity;

    sprite_transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);

    if (!sprite_transfer_buffer_)
    {
        LogPrint("GpuImmediate: SDL_CreateGPUTransferBuffer (sprite) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUBuffer(device_, sprite_buffer_);
        sprite_buffer_ = nullptr;
        return false;
    }

    sprite_buffer_capacity_ = capacity;

    return true;
}

void GpuImmediate::UploadSpriteInstances()
{
    if (sprite_instance_count_ == 0)
        return;

    size_t bytes = (size_t)sprite_instance_count_ * sizeof(SpriteInstance);

    if (!EnsureSpriteCapacity(bytes))
        return;

    void *mapped = SDL_MapGPUTransferBuffer(device_, sprite_transfer_buffer_, true);

    if (!mapped)
    {
        LogPrint("GpuImmediate: SDL_MapGPUTransferBuffer (sprite) failed: %s\n", SDL_GetError());
        return;
    }

    memcpy(mapped, sprite_instances_.data(), bytes);

    SDL_UnmapGPUTransferBuffer(device_, sprite_transfer_buffer_);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(gpu_device.CommandBuffer());

    SDL_GPUTransferBufferLocation source;
    source.transfer_buffer = sprite_transfer_buffer_;
    source.offset          = 0;

    SDL_GPUBufferRegion destination;
    destination.buffer = sprite_buffer_;
    destination.offset = 0;
    destination.size   = (uint32_t)bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

    SDL_EndGPUCopyPass(copy_pass);

    uploaded_bytes_ += bytes;
}

void GpuImmediate::BindFragmentTextures(SDL_GPURenderPass *pass, SDL_GPUTexture *const texture[4],
                                        SDL_GPUSampler *const sampler[4])
{
    if (bound_texture_[0] == texture[0] && bound_sampler_[0] == sampler[0] && bound_texture_[1] == texture[1] &&
        bound_sampler_[1] == sampler[1] && bound_texture_[2] == texture[2] && bound_sampler_[2] == sampler[2] &&
        bound_texture_[3] == texture[3] && bound_sampler_[3] == sampler[3])
        return;

    SDL_GPUTextureSamplerBinding bindings[4];

    bindings[0].texture = texture[0] ? texture[0] : default_texture_;
    bindings[0].sampler = sampler[0] ? sampler[0] : default_sampler_;
    bindings[1].texture = texture[1] ? texture[1] : default_texture_;
    bindings[1].sampler = sampler[1] ? sampler[1] : default_sampler_;
    bindings[2].texture = texture[2];
    bindings[2].sampler = sampler[2];
    bindings[3].texture = texture[3];
    bindings[3].sampler = sampler[3];

    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);

    for (int b = 0; b < 4; b++)
    {
        bound_texture_[b] = texture[b];
        bound_sampler_[b] = sampler[b];
    }

    binding_count_++;
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

void GpuImmediate::ResetTargetRectangles()
{
    current_viewport_.x      = 0;
    current_viewport_.y      = 0;
    current_viewport_.width  = gpu_device.CurrentTargetWidth();
    current_viewport_.height = gpu_device.CurrentTargetHeight();

    current_scissor_ = current_viewport_;

    viewport_set_ = true;
    scissor_set_  = true;
}

void GpuImmediate::ApplyPassState()
{
    SDL_GPURenderPass *pass = gpu_device.RenderPass();

    GpuBindLightBuffers(pass);

    bound_pipeline_                 = nullptr;
    bound_texture_[0]               = nullptr;
    bound_texture_[1]               = nullptr;
    bound_sampler_[0]               = nullptr;
    bound_sampler_[1]               = nullptr;
    bound_index_buffer_             = nullptr;
    bound_vertex_buffer_            = nullptr;
    bound_vertex_parameter_index_   = -1;
    bound_fragment_parameter_index_ = -1;
    bound_light_table_index_        = -1;
    bound_stencil_reference_        = -1;

    if (!pass)
        return;

    if (vertex_buffer_)
    {
        SDL_GPUBufferBinding binding;
        binding.buffer = vertex_buffer_;
        binding.offset = 0;

        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

        bound_vertex_buffer_ = vertex_buffer_;
    }

    int32_t target_height = gpu_device.CurrentTargetHeight();

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
        int32_t target_width = gpu_device.CurrentTargetWidth();

        int32_t left   = epi::Max(0, current_scissor_.x);
        int32_t bottom = epi::Max(0, current_scissor_.y);
        int32_t right  = epi::Min(target_width, current_scissor_.x + current_scissor_.width);
        int32_t top    = epi::Min(target_height, current_scissor_.y + current_scissor_.height);

        SDL_Rect rectangle;
        rectangle.x = left;
        rectangle.y = target_height - top;
        rectangle.w = epi::Max(0, right - left);
        rectangle.h = epi::Max(0, top - bottom);

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

    if (!gpu_device.ReplayTargetReady())
        return;

    viewport_set_ = false;
    scissor_set_  = false;

    UploadVertices();
    UploadIndices();
    UploadSpriteInstances();
    GpuFlushLightBuffers();

    gpu_device.BeginPass(kGpuLoadOperationClear, kGpuLoadOperationClear, kGpuLoadOperationClear);

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
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationClear, kGpuLoadOperationLoad);
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
            for (int32_t binding_index = 0; binding_index < 3; binding_index++)
            {
                movie_bindings[binding_index].texture = movie->texture[binding_index];
                movie_bindings[binding_index].sampler = movie->sampler;
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
            for (int32_t plane_index = 0; plane_index < 4; plane_index++)
                movie_fragment.plane_scales[plane_index] = movie->plane_scales[plane_index];

            SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuVertexUniformSlot, &movie_vertex,
                                         (uint32_t)sizeof(movie_vertex));
            SDL_PushGPUFragmentUniformData(gpu_device.CommandBuffer(), kGpuFragmentUniformSlot, &movie_fragment,
                                           (uint32_t)sizeof(movie_fragment));
            uniform_push_count_ += 2;

            bound_vertex_parameter_index_   = -1;
            bound_fragment_parameter_index_ = -1;

            if (vertex_buffer_ && bound_vertex_buffer_ != vertex_buffer_)
            {
                SDL_GPUBufferBinding movie_vertex_binding;
                movie_vertex_binding.buffer = vertex_buffer_;
                movie_vertex_binding.offset = 0;

                SDL_BindGPUVertexBuffers(pass, 0, &movie_vertex_binding, 1);

                bound_vertex_buffer_ = vertex_buffer_;
                binding_count_++;
            }

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

        if (command->type == kGpuCommandModelDraw)
        {
            const GpuModelDrawArguments *model = &command->arguments.model_draw;

            SDL_GPURenderPass *pass = gpu_device.RenderPass();

            if (!pass)
            {
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationClear, kGpuLoadOperationLoad);
                ApplyPassState();
                pass = gpu_device.RenderPass();
            }

            if (!pass)
                continue;

            SDL_BindGPUGraphicsPipeline(pass, model->pipeline);
            bound_pipeline_ = model->pipeline;
            pipeline_bind_count_++;

            SDL_SetGPUStencilReference(pass, model->stencil_reference);
            bound_stencil_reference_ = model->stencil_reference;

            SDL_GPUTextureSamplerBinding model_bindings[2];
            model_bindings[0].texture = model->texture;
            model_bindings[0].sampler = model->sampler;
            model_bindings[1].texture = model->lookup_texture;
            model_bindings[1].sampler = model->lookup_sampler;

            SDL_BindGPUFragmentSamplers(pass, 0, model_bindings, 2);
            binding_count_++;

            for (int b = 0; b < 4; b++)
            {
                bound_texture_[b] = nullptr;
                bound_sampler_[b] = nullptr;
            }

            SDL_GPUBufferBinding vertex_bindings[5];

            vertex_bindings[kGpuModelBufferSlotPositionFrame1].buffer = model->position_buffer;
            vertex_bindings[kGpuModelBufferSlotPositionFrame1].offset = model->position_frame1_offset;

            vertex_bindings[kGpuModelBufferSlotPositionFrame2].buffer = model->position_buffer;
            vertex_bindings[kGpuModelBufferSlotPositionFrame2].offset = model->position_frame2_offset;

            vertex_bindings[kGpuModelBufferSlotTextureCoordinates].buffer = model->texture_coordinate_buffer;
            vertex_bindings[kGpuModelBufferSlotTextureCoordinates].offset = model->texture_coordinate_offset;

            vertex_bindings[kGpuModelBufferSlotNormalFrame1].buffer = model->normal_buffer;
            vertex_bindings[kGpuModelBufferSlotNormalFrame1].offset = model->normal_frame1_offset;

            vertex_bindings[kGpuModelBufferSlotNormalFrame2].buffer = model->normal_buffer;
            vertex_bindings[kGpuModelBufferSlotNormalFrame2].offset = model->normal_frame2_offset;

            SDL_BindGPUVertexBuffers(pass, 0, vertex_bindings, 5);
            binding_count_++;

            SDL_GPUBufferBinding model_index_binding;
            model_index_binding.buffer = model->index_buffer;
            model_index_binding.offset = 0;

            SDL_BindGPUIndexBuffer(pass, &model_index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
            bound_index_buffer_ = model->index_buffer;
            binding_count_++;

            SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuVertexUniformSlot,
                                         &model_vertex_parameters_[(size_t)model->vertex_parameter_index],
                                         (uint32_t)sizeof(GpuModelVertexParameters));

            SDL_PushGPUFragmentUniformData(gpu_device.CommandBuffer(), kGpuFragmentUniformSlot,
                                           &model_fragment_parameters_[(size_t)model->fragment_parameter_index],
                                           (uint32_t)sizeof(GpuModelFragmentParameters));

            uniform_push_count_ += 2;
            uniform_bytes_ += sizeof(GpuModelVertexParameters) + sizeof(GpuModelFragmentParameters);

            if (bound_light_table_index_ != model->light_table_index)
            {
                SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuSpriteUniformSlot,
                                             &sprite_light_tables_[(size_t)model->light_table_index],
                                             (uint32_t)sizeof(SpriteLightTable));

                bound_light_table_index_ = model->light_table_index;
                uniform_push_count_++;
                uniform_bytes_ += sizeof(SpriteLightTable);
            }

            bound_vertex_parameter_index_   = -1;
            bound_fragment_parameter_index_ = -1;

            SDL_DrawGPUIndexedPrimitives(pass, (uint32_t)model->index_count, 1, (uint32_t)model->index_first, 0, 0);

            draw_count_++;

            bound_vertex_buffer_ = nullptr;

            if (vertex_buffer_)
            {
                SDL_GPUBufferBinding world_binding;
                world_binding.buffer = vertex_buffer_;
                world_binding.offset = 0;

                SDL_BindGPUVertexBuffers(pass, 0, &world_binding, 1);
                bound_vertex_buffer_ = vertex_buffer_;
                binding_count_++;
            }

            continue;
        }


        if (command->type == kGpuCommandSpriteDraw)
        {
            SDL_GPURenderPass *pass = gpu_device.RenderPass();

            const GpuSpriteDrawArguments *sprite = &command->arguments.sprite_draw;

            SDL_GPUBuffer *instance_buffer = sprite->buffer ? sprite->buffer : sprite_buffer_;

            if (!pass || !instance_buffer)
                continue;

            if (bound_pipeline_ != sprite->pipeline)
            {
                SDL_BindGPUGraphicsPipeline(pass, sprite->pipeline);
                bound_pipeline_ = sprite->pipeline;
                pipeline_bind_count_++;
            }

            if (bound_stencil_reference_ != sprite->stencil_reference)
            {
                SDL_SetGPUStencilReference(pass, sprite->stencil_reference);
                bound_stencil_reference_ = sprite->stencil_reference;
            }

            BindFragmentTextures(pass, sprite->texture, sprite->sampler);

            if (bound_vertex_parameter_index_ != sprite->vertex_parameter_index)
            {
                SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuVertexUniformSlot,
                                             &vertex_parameters_[sprite->vertex_parameter_index],
                                             (uint32_t)sizeof(GpuVertexParameters));

                bound_vertex_parameter_index_ = sprite->vertex_parameter_index;
                uniform_push_count_++;
                uniform_bytes_ += sizeof(GpuVertexParameters);
            }

            if (bound_fragment_parameter_index_ != sprite->fragment_parameter_index)
            {
                SDL_PushGPUFragmentUniformData(gpu_device.CommandBuffer(), kGpuFragmentUniformSlot,
                                               &fragment_parameters_[sprite->fragment_parameter_index],
                                               (uint32_t)sizeof(GpuFragmentParameters));

                bound_fragment_parameter_index_ = sprite->fragment_parameter_index;
                uniform_push_count_++;
                uniform_bytes_ += sizeof(GpuFragmentParameters);
            }

            if (bound_light_table_index_ != sprite->light_table_index)
            {
                SDL_PushGPUVertexUniformData(gpu_device.CommandBuffer(), kGpuSpriteUniformSlot,
                                             &sprite_light_tables_[sprite->light_table_index],
                                             (uint32_t)sizeof(SpriteLightTable));

                bound_light_table_index_ = sprite->light_table_index;
                uniform_push_count_++;
                uniform_bytes_ += sizeof(SpriteLightTable);
            }

            if (bound_vertex_buffer_ != instance_buffer)
            {
                SDL_GPUBufferBinding sprite_binding;
                sprite_binding.buffer = instance_buffer;
                sprite_binding.offset = 0;

                SDL_BindGPUVertexBuffers(pass, 0, &sprite_binding, 1);

                bound_vertex_buffer_ = instance_buffer;
                binding_count_++;
            }

            if (bound_index_buffer_ != quad_index_buffer_)
            {
                SDL_GPUBufferBinding sprite_index_binding;
                sprite_index_binding.buffer = quad_index_buffer_;
                sprite_index_binding.offset = 0;

                SDL_BindGPUIndexBuffer(pass, &sprite_index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                bound_index_buffer_ = quad_index_buffer_;
                binding_count_++;
            }

            SDL_DrawGPUIndexedPrimitives(pass, 6, (uint32_t)sprite->instance_count, 0, 0,
                                         (uint32_t)sprite->instance_first);

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
            else if (command->type == kGpuCommandBeginWorldTarget)
            {
                gpu_device.SetWorldDirect(command->arguments.resolve.direct);

                gpu_device.BeginPass(gpu_device.WorldDirect() ? kGpuLoadOperationLoad : kGpuLoadOperationClear,
                                     kGpuLoadOperationClear, kGpuLoadOperationClear, kGpuPassTargetWorld);

                ResetTargetRectangles();
            }
            else if (command->type == kGpuCommandBeginOitTarget)
            {
                gpu_device.BeginPass(kGpuLoadOperationClear, kGpuLoadOperationLoad, kGpuLoadOperationLoad,
                                     kGpuPassTargetOit);
            }
            else if (command->type == kGpuCommandEndOitTarget)
            {
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationLoad, kGpuLoadOperationLoad,
                                     kGpuPassTargetWorld);
            }
            else if (command->type == kGpuCommandResolveWorldTarget)
            {
                const GpuResolveArguments *resolve = &command->arguments.resolve;

                GpuBlitRectangle source;
                source.x      = resolve->source_x;
                source.y      = resolve->source_y;
                source.width  = resolve->source_width;
                source.height = resolve->source_height;

                GpuBlitRectangle destination;
                destination.x      = resolve->destination_x;
                destination.y      = resolve->destination_y;
                destination.width  = resolve->destination_width;
                destination.height = resolve->destination_height;

                if (gpu_device.WorldDirect())
                    gpu_device.SetWorldDirect(false);
                else
                    gpu_device.BlitWorldToMain(source, destination, resolve->smooth);

                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationLoad, kGpuLoadOperationLoad,
                                     kGpuPassTargetMain);

                ResetTargetRectangles();
            }
            else if (command->type == kGpuCommandClearStencil)
            {
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationLoad, kGpuLoadOperationClear,
                                     gpu_device.CurrentTarget());
            }
            else
            {
                gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationClear, kGpuLoadOperationLoad,
                                     gpu_device.CurrentTarget());
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

        if (bound_stencil_reference_ != draw->stencil_reference)
        {
            SDL_SetGPUStencilReference(pass, draw->stencil_reference);
            bound_stencil_reference_ = draw->stencil_reference;
        }

        BindFragmentTextures(pass, draw->texture, draw->sampler);

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

        SDL_GPUBuffer *wanted_vertex_buffer = draw->vertex_buffer ? draw->vertex_buffer : vertex_buffer_;

        if (wanted_vertex_buffer && bound_vertex_buffer_ != wanted_vertex_buffer)
        {
            SDL_GPUBufferBinding vertex_binding;
            vertex_binding.buffer = wanted_vertex_buffer;
            vertex_binding.offset = 0;

            SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);

            bound_vertex_buffer_ = wanted_vertex_buffer;
            binding_count_++;
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
